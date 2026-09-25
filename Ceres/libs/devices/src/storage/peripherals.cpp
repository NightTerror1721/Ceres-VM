#include <ceres/devices/storage/peripherals.h>
#include <algorithm>
#include <fstream>

namespace ceres::devices
{
	namespace
	{
		// Every register of the device (plan/v2 SPEC 5.3), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x00, "Status",       RegisterAccess::Read,      0x0, false, "EventPending, Ready, Error." },
			{ 0x04, "PortCount",    RegisterAccess::Read,      0x0, false, "How many ports there are." },
			{ 0x08, "PortSelect",   RegisterAccess::ReadWrite, 0x0, false, "The port the registers below are about." },
			{ 0x0C, "Event",        RegisterAccess::Read,      0x0, true,  "Takes the next event off the queue, or 0 when there is none." },
			{ 0x10, "Command",      RegisterAccess::Write,     0x0, false, "CommandEject or CommandFlush, for the selected port." },
			{ 0x20, "PortStatus",   RegisterAccess::Read,      0x0, false, "Present, WriteProtected, Error, for the selected port." },
			{ 0x24, "PortType",     RegisterAccess::Read,      0x0, false, "TypeNone, TypeStorage or TypeCartridge." },
			{ 0x28, "PortId",       RegisterAccess::Read,      0x0, false, "An identifier of the medium (0 when there is none)." },
			{ 0x2C, "PortSectors",  RegisterAccess::Read,      0x0, false, "How many sectors the medium has." },
			{ 0x30, "Sector",       RegisterAccess::ReadWrite, 0x0, false, "The sector a transfer is about." },
			{ 0xF0, "BlockAddress", RegisterAccess::Write,     0x0, false, "RAM address of the sector buffer, for the selected port." },
			{ 0xF4, "BlockLength",  RegisterAccess::Write,     0x0, false, "Bytes to move: at most one sector." },
			{ 0xF8, "BlockCommand", RegisterAccess::Write,     0x0, false, "1 reads the sector into RAM, 2 writes RAM to the sector." },
		};
	}

	PeripheralDevice::~PeripheralDevice()
	{
		// Media that were still plugged in when the machine ended keep what was written to them
		for (Port& port : _ports)
			flushPort(port);
	}

	bool PeripheralDevice::attachImage(u32 port, std::string name, std::vector<u8> image, Kind kind, std::filesystem::path backingFile, bool writeProtected)
	{
		if (port >= PortCount)
			return false;
		{
			std::lock_guard<std::mutex> lock(_lock);
			Port& slot = _ports[port];
			if (slot.present)
				return false;
			if (image.size() % SectorSize != 0)
				image.resize(((image.size() / SectorSize) + 1) * SectorSize, 0);
			slot.present = true;
			slot.kind = kind;
			slot.writeProtected = writeProtected || kind == Kind::Cartridge;
			slot.name = std::move(name);
			slot.path = std::move(backingFile);
			slot.image = std::move(image);
			slot.dirty = false;
			_events.push_back(Event{ port, EventConnected });
		}
		raiseInterrupt(Interrupt);
		return true;
	}

	bool PeripheralDevice::attachFile(u32 port, const std::filesystem::path& path, Kind kind, std::string* error, bool writeProtected)
	{
		auto fail = [&](std::string message)
		{
			if (error)
				*error = std::move(message);
			return false;
		};
		if (port >= PortCount)
			return fail("there is no port " + std::to_string(port) + " (ports are 0 to " + std::to_string(PortCount - 1) + ")");
		if (isPresent(port))
			return fail("port " + std::to_string(port) + " already has something plugged in");

		std::vector<u8> image;
		std::error_code ec;
		if (std::filesystem::exists(path, ec))
		{
			std::ifstream file(path, std::ios::binary);
			if (!file)
				return fail("cannot read " + path.string());
			image.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		}
		else if (kind == Kind::Cartridge)
			return fail("the cartridge file " + path.string() + " does not exist");
		else
			image.assign(static_cast<usize>(64) * SectorSize, 0);
		if (kind == Kind::Cartridge && image.empty())
			return fail("the cartridge file " + path.string() + " is empty");

		if (!attachImage(port, path.filename().string(), std::move(image), kind, path, writeProtected))
			return fail("port " + std::to_string(port) + " could not take it");
		return true;
	}

	int PeripheralDevice::attachToFreePort(const std::filesystem::path& path, Kind kind, std::string* error)
	{
		for (u32 port = 0; port < PortCount; ++port)
		{
			if (isPresent(port))
				continue;
			return attachFile(port, path, kind, error) ? static_cast<int>(port) : -1;
		}
		if (error)
			*error = "every port is taken";
		return -1;
	}

	bool PeripheralDevice::detach(u32 port)
	{
		if (port >= PortCount)
			return false;
		{
			std::lock_guard<std::mutex> lock(_lock);
			Port& slot = _ports[port];
			if (!slot.present)
				return false;
			flushPort(slot);
			slot = Port{};
			_events.push_back(Event{ port, EventDisconnected });
		}
		raiseInterrupt(Interrupt);
		return true;
	}

	void PeripheralDevice::setWriteProtected(u32 port, bool protectedMedium)
	{
		std::lock_guard<std::mutex> lock(_lock);
		if (port < PortCount && _ports[port].present)
			_ports[port].writeProtected = protectedMedium || _ports[port].kind == Kind::Cartridge;
	}

	bool PeripheralDevice::isPresent(u32 port) const
	{
		std::lock_guard<std::mutex> lock(_lock);
		return port < PortCount && _ports[port].present;
	}

	u32 PeripheralDevice::sectorCount(u32 port) const
	{
		std::lock_guard<std::mutex> lock(_lock);
		return port < PortCount ? static_cast<u32>(_ports[port].image.size() / SectorSize) : 0;
	}

	std::vector<u8> PeripheralDevice::imageOf(u32 port) const
	{
		std::lock_guard<std::mutex> lock(_lock);
		return port < PortCount ? _ports[port].image : std::vector<u8>{};
	}

	usize PeripheralDevice::pendingEvents() const
	{
		std::lock_guard<std::mutex> lock(_lock);
		return _events.size();
	}

	std::string PeripheralDevice::describe(u32 port) const
	{
		std::lock_guard<std::mutex> lock(_lock);
		if (port >= PortCount)
			return "no such port";
		const Port& slot = _ports[port];
		if (!slot.present)
			return "empty";
		return std::string(slot.kind == Kind::Cartridge ? "cartridge " : "storage ") + slot.name + " (" +
			std::to_string(slot.image.size() / SectorSize) + " sectors" + (slot.writeProtected ? ", write protected" : "") + ")";
	}

	bool PeripheralDevice::flushPort(Port& port)
	{
		if (!port.present || port.path.empty() || !port.dirty)
			return true;
		std::ofstream file(port.path, std::ios::binary | std::ios::trunc);
		if (!file)
			return false;
		file.write(reinterpret_cast<const char*>(port.image.data()), static_cast<std::streamsize>(port.image.size()));
		port.dirty = false;
		return true;
	}

	u32 PeripheralDevice::identifierOf(const Port& port)
	{
		u32 hash = 2166136261u;
		for (char c : port.name)
			hash = (hash ^ static_cast<u8>(c)) * 16777619u;
		hash = (hash ^ static_cast<u32>(port.image.size())) * 16777619u;
		return hash == 0 ? 1u : hash;   // 0 is what "nothing plugged in" reads
	}

	bool PeripheralDevice::transfer(bool toRam)
	{
		Port& port = _ports[_selected];
		if (!port.present)
			return false;
		if (!toRam && port.writeProtected)
			return false;
		if (_blockLength == 0)
			return true;

		const usize offset = static_cast<usize>(_sector) * SectorSize;
		if (offset >= port.image.size() || _blockLength > SectorSize)
			return false;
		const u32 available = static_cast<u32>(std::min<usize>(_blockLength, port.image.size() - offset));
		const u32 clamped = memory().clampBlockSize(Address(_blockAddress), available);
		if (clamped == 0)
			return false;

		if (toRam)
		{
			auto buffer = memory().peekMutBytes(Address(_blockAddress), clamped);
			std::copy_n(port.image.begin() + static_cast<std::ptrdiff_t>(offset), clamped, buffer.begin());
		}
		else
		{
			const auto bytes = memory().peekBytes(Address(_blockAddress), clamped);
			std::copy_n(bytes.begin(), clamped, port.image.begin() + static_cast<std::ptrdiff_t>(offset));
			port.dirty = true;
		}
		return true;
	}

	u32 PeripheralDevice::read(Address offset)
	{
		std::lock_guard<std::mutex> lock(_lock);
		const Port& port = _ports[_selected];

		if (offset == StatusRegister) return statusLocked();
		if (offset == PortCountRegister) return PortCount;
		if (offset == PortSelectRegister) return _selected;
		if (offset == EventRegister)
		{
			if (_events.empty())
				return 0;
			const Event event = _events.front();
			_events.pop_front();
			return EventValid | (event.kind << 8) | event.port;
		}
		if (offset == PortStatusRegister)
			return (port.present ? PortPresent : 0u) | (port.present && port.writeProtected ? PortWriteProtected : 0u) | (_error ? PortError : 0u);
		if (offset == PortTypeRegister) return port.present ? static_cast<u32>(port.kind) : TypeNone;
		if (offset == PortIdRegister) return port.present ? identifierOf(port) : 0u;
		if (offset == PortSectorsRegister) return port.present ? static_cast<u32>(port.image.size() / SectorSize) : 0u;
		if (offset == SectorRegister) return _sector;
		return 0;
	}

	void PeripheralDevice::write(Address offset, u32 value)
	{
		std::lock_guard<std::mutex> lock(_lock);

		if (offset == PortSelectRegister)
		{
			if (value < PortCount)
			{
				_selected = value;
				_error = false;
			}
			else
				_error = true;   // a port that is not there: the selection stays where it was
			return;
		}
		if (offset == SectorRegister) { _sector = value; return; }
		if (offset == BlockAddressRegister) { _blockAddress = value; return; }
		if (offset == BlockLengthRegister) { _blockLength = value; return; }
		if (offset == BlockCommandRegister)
		{
			if (value == BlockCommandRead)
				_error = !transfer(true);
			else if (value == BlockCommandWrite)
				_error = !transfer(false);
			else
				_error = true;
			return;
		}
		if (offset == CommandRegister)
		{
			Port& port = _ports[_selected];
			if (value == CommandFlush)
				_error = !port.present || !flushPort(port);
			else if (value == CommandEject && port.present)
			{
				flushPort(port);
				port = Port{};
				_events.push_back(Event{ _selected, EventDisconnected });
				_error = false;
				raiseInterrupt(Interrupt);   // from the machine's own thread, like every other device that interrupts
			}
			else
				_error = true;
		}
	}

	const RegisterMap& PeripheralDevice::registers() const
	{
		static constexpr RegisterMap map{ "peripherals", Registers };
		return map;
	}
}

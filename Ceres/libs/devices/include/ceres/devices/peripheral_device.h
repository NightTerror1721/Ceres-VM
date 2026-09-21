#pragma once

// Things that are plugged in while the machine runs: a small controller with a few ports, where the host connects
// and disconnects media - a memory stick, a cartridge - and the program is told when it happens.
//
// The disk in slot 2 stays the machine's own internal drive, fixed for the life of the run. This is the other
// thing: what a person plugs in. A port holds nothing, a storage medium (sectors it can read and write, like the
// disk) or a cartridge (sectors it can only read). Connecting or disconnecting one queues an event and raises the
// device's interrupt, and the program reads the events, asks the selected port what it holds, and moves sectors in
// and out of RAM through the same block registers the disk has.
//
//   li   r1, 0
//   str  [r_dev + PORT_SELECT], r1        // look at port 0
//   ldr  r2, [r_dev + PORT_STATUS]        // bit 0: is something plugged in?
//   ldr  r3, [r_dev + PORT_SECTORS]       // how many 512-byte sectors it has
//   str  [r_dev + SECTOR], r0             // sector 0
//   str  [r_dev + BLOCK_ADDR], r_buffer
//   li   r4, 512
//   str  [r_dev + BLOCK_LEN], r4
//   li   r5, 1
//   str  [r_dev + BLOCK_CMD], r5          // 1 = read sector -> RAM

#include <ceres/vm/mmio_bus.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ceres::devices
{
	using namespace vm;

	class PeripheralDevice final : public IODevice
	{
	public:
		static inline constexpr Address StatusRegister = Address(0x00);     // Read: EventPending, Ready, Error.
		static inline constexpr Address PortCountRegister = Address(0x04);  // Read: how many ports there are.
		static inline constexpr Address PortSelectRegister = Address(0x08); // Read/write: the port the registers below are about.
		static inline constexpr Address EventRegister = Address(0x0C);      // Read: takes the next event off the queue, or 0 when there is none.
		static inline constexpr Address CommandRegister = Address(0x10);    // Write: CommandEject or CommandFlush, for the selected port.
		static inline constexpr Address PortStatusRegister = Address(0x20); // Read: Present, WriteProtected, Error, for the selected port.
		static inline constexpr Address PortTypeRegister = Address(0x24);   // Read: TypeNone, TypeStorage or TypeCartridge.
		static inline constexpr Address PortIdRegister = Address(0x28);     // Read: an identifier of the medium (0 when there is none).
		static inline constexpr Address PortSectorsRegister = Address(0x2C);// Read: how many sectors the medium has.
		static inline constexpr Address SectorRegister = Address(0x30);     // Read/write: the sector a transfer is about.
		static inline constexpr Address BlockAddressRegister = Address(0xF0);
		static inline constexpr Address BlockLengthRegister = Address(0xF4);
		static inline constexpr Address BlockCommandRegister = Address(0xF8);

		static inline constexpr u32 PortCount = 4;
		static inline constexpr u32 SectorSize = 512;

		// The eighth user interrupt (the timer, terminal, DMA controller, keyboard, mouse, gamepad and audio have 0-6):
		// something was connected or disconnected. It is raised for each event, so a handler that reads one event per
		// interrupt sees them all; reading until EventRegister gives 0 is the same thing.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt7;

		// StatusRegister
		static inline constexpr u32 StatusEventPending = 1u << 0;
		static inline constexpr u32 StatusReady = 1u << 1;   // the last operation went through
		static inline constexpr u32 StatusError = 1u << 2;   // it did not: a bad port, nothing plugged in, a write to a protected medium

		// PortStatusRegister
		static inline constexpr u32 PortPresent = 1u << 0;
		static inline constexpr u32 PortWriteProtected = 1u << 1;
		static inline constexpr u32 PortError = 1u << 3;

		// PortTypeRegister
		static inline constexpr u32 TypeNone = 0;
		static inline constexpr u32 TypeStorage = 1;     // sectors that can be read and written
		static inline constexpr u32 TypeCartridge = 2;   // sectors that can only be read

		// EventRegister: bit 31 says there is one, bits 15:8 what happened, bits 7:0 to which port.
		static inline constexpr u32 EventValid = 1u << 31;
		static inline constexpr u32 EventConnected = 1;
		static inline constexpr u32 EventDisconnected = 2;

		static inline constexpr u32 CommandEject = 1;    // disconnect the selected port's medium, as if it had been pulled out
		static inline constexpr u32 CommandFlush = 2;    // write the selected medium's changes back to its file

		static inline constexpr u32 BlockCommandRead = 1;   // sector -> RAM
		static inline constexpr u32 BlockCommandWrite = 2;  // RAM -> sector

		enum class Kind : u32 { Storage = TypeStorage, Cartridge = TypeCartridge };

		struct Event
		{
			u32 port = 0;
			u32 kind = 0;   // EventConnected or EventDisconnected
		};

	private:
		struct Port
		{
			bool present = false;
			Kind kind = Kind::Storage;
			bool writeProtected = false;
			std::string name;
			std::filesystem::path path;   // empty for a medium that lives only in memory
			std::vector<u8> image;
			bool dirty = false;
		};

		mutable std::mutex _lock;   // the host connects from its own thread while the machine runs
		std::array<Port, PortCount> _ports;
		std::deque<Event> _events;
		u32 _selected = 0;
		u32 _sector = 0;
		u32 _blockAddress = 0;
		u32 _blockLength = 0;
		bool _error = false;

	public:
		PeripheralDevice() = default;
		PeripheralDevice(const PeripheralDevice&) = delete;
		PeripheralDevice(PeripheralDevice&&) = delete;
		~PeripheralDevice() override
		{
			// Media that were still plugged in when the machine ended keep what was written to them
			for (Port& port : _ports)
				flushPort(port);
		}

		PeripheralDevice& operator=(const PeripheralDevice&) = delete;
		PeripheralDevice& operator=(PeripheralDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus) { bus.attach(default_mmio::Peripherals, *this); }
		void detachFrom(MmioBus& bus) { bus.detach(default_mmio::Peripherals); }

		// ---- the host's side --------------------------------------------------------------------------------------

		// Plugs a medium with these contents into `port`. False when there is no such port or something is already in
		// it: a second connection is refused, not replaced. A storage medium is padded to a whole number of sectors;
		// a cartridge is always write protected.
		bool attachImage(u32 port, std::string name, std::vector<u8> image, Kind kind, std::filesystem::path backingFile = {}, bool writeProtected = false)
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

		// Plugs in the host file at `path`. A storage medium whose file is not there yet is created with 64 sectors,
		// as the disk does; a cartridge has to exist and hold something. False, with `error` saying why, when that
		// cannot be done.
		bool attachFile(u32 port, const std::filesystem::path& path, Kind kind, std::string* error = nullptr, bool writeProtected = false)
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

		// Plugs a file into the first port that is free, and says which; -1 when they are all taken or it will not open.
		int attachToFreePort(const std::filesystem::path& path, Kind kind, std::string* error = nullptr)
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

		// Pulls the medium out of `port`, keeping what was written to it. False when there was nothing there.
		bool detach(u32 port)
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

		void setWriteProtected(u32 port, bool protectedMedium)
		{
			std::lock_guard<std::mutex> lock(_lock);
			if (port < PortCount && _ports[port].present)
				_ports[port].writeProtected = protectedMedium || _ports[port].kind == Kind::Cartridge;
		}

		bool isPresent(u32 port) const
		{
			std::lock_guard<std::mutex> lock(_lock);
			return port < PortCount && _ports[port].present;
		}

		u32 sectorCount(u32 port) const
		{
			std::lock_guard<std::mutex> lock(_lock);
			return port < PortCount ? static_cast<u32>(_ports[port].image.size() / SectorSize) : 0;
		}

		// A copy of the medium's bytes, for a test or a debugger to look at.
		std::vector<u8> imageOf(u32 port) const
		{
			std::lock_guard<std::mutex> lock(_lock);
			return port < PortCount ? _ports[port].image : std::vector<u8>{};
		}

		usize pendingEvents() const
		{
			std::lock_guard<std::mutex> lock(_lock);
			return _events.size();
		}

		// What the port holds, in words: "empty", "storage stick.img (64 sectors)".
		std::string describe(u32 port) const
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

	private:
		static bool flushPort(Port& port)
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

		// FNV-1a of the medium's name, mixed with its size: the same file is the same identifier every time, and two
		// different ones almost never share it.
		static u32 identifierOf(const Port& port)
		{
			u32 hash = 2166136261u;
			for (char c : port.name)
				hash = (hash ^ static_cast<u8>(c)) * 16777619u;
			hash = (hash ^ static_cast<u32>(port.image.size())) * 16777619u;
			return hash == 0 ? 1u : hash;   // 0 is what "nothing plugged in" reads
		}

		u32 statusLocked() const
		{
			return (_events.empty() ? 0u : StatusEventPending) | (_error ? StatusError : StatusReady);
		}

		// `size` bytes of the selected medium's selected sector, to or from RAM at `ramAddress`. A transfer that runs
		// off the end of the sector or of RAM is clamped, as with the disk.
		bool transfer(bool toRam)
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

	public:
		// ---- the program's side -----------------------------------------------------------------------------------

		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedWord(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedWord(offset)); }

		u32 readUnsignedWord(Address offset) override
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

		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }

		void writeWord(Address offset, u32 value) override
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
	};
}

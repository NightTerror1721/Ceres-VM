#include <ceres/devices/storage/disk.h>
#include <algorithm>
#include <fstream>

namespace ceres::devices
{
	namespace
	{
		// Every register of the device (plan/v2 SPEC 5.3), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x00, "Status",       RegisterAccess::Read,      0x0, false, "Bit 0 ready, bit 1 the last transfer failed (a sector outside the disk, a bad length)." },
			{ 0x04, "Command",      RegisterAccess::Write,     0x0, false, "1 flushes the disk to its host file." },
			{ 0x08, "Sector",       RegisterAccess::ReadWrite, 0x0, false, "The sector the next block transfer reads or writes." },
			{ 0x0C, "SectorCount",  RegisterAccess::Read,      0x0, false, "How many sectors the disk has." },
			{ 0xF0, "BlockAddress", RegisterAccess::Write,     0x0, false, "RAM address of the sector buffer." },
			{ 0xF4, "BlockLength",  RegisterAccess::Write,     0x0, false, "Bytes to move: at most one sector." },
			{ 0xF8, "BlockCommand", RegisterAccess::Write,     0x0, false, "1 reads the sector into RAM, 2 writes RAM to the sector." },
		};
	}

	DiskDevice::~DiskDevice()
	{
		// A program that forgets to flush still gets its writes; losing them would make the
		// device useless for the one thing it is for.
		flush();
	}

	bool DiskDevice::open(const std::filesystem::path& path, u32 sectors)
	{
		std::error_code error;
		if (std::filesystem::exists(path, error))
		{
			std::ifstream file(path, std::ios::binary);
			if (!file)
				return false;

			_image.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		}
		else
		{
			_image.assign(static_cast<usize>(sectors) * SectorSize, 0);
		}

		// A short file is padded rather than refused: a disk with a partial last sector is a
		// disk, and rounding it up is what every image format does anyway.
		if (_image.size() % SectorSize != 0)
			_image.resize(((_image.size() / SectorSize) + 1) * SectorSize, 0);

		_path = path;
		_status = StatusReady;
		_dirty = false;
		return true;
	}

	bool DiskDevice::flush()
	{
		if (_path.empty() || !_dirty)
			return true;

		std::ofstream file(_path, std::ios::binary | std::ios::trunc);
		if (!file)
			return false;

		file.write(reinterpret_cast<const char*>(_image.data()), static_cast<std::streamsize>(_image.size()));
		_dirty = false;
		return true;
	}

	void DiskDevice::blockRead(Address ramAddress, u32 size)
	{
		if (size == 0)
			return;

		const usize offset = static_cast<usize>(_sector) * SectorSize;
		if (offset >= _image.size() || size > SectorSize)
		{
			_status = StatusError;
			return;
		}

		// A transfer that runs off the end of the sector is clamped rather than reading the
		// next one: a sector is the unit, and silently spilling into its neighbour is how a
		// program ends up with data it never asked for. So is one that runs off the end of RAM.
		const u32 available = static_cast<u32>(std::min<usize>(size, _image.size() - offset));
		const u32 clampSize = memory().clampBlockSize(ramAddress, available);
		if (clampSize == 0)
		{
			_status = StatusError;
			return;
		}

		auto buffer = memory().peekMutBytes(ramAddress, clampSize);
		std::copy_n(_image.begin() + static_cast<std::ptrdiff_t>(offset), clampSize, buffer.begin());
		_status = StatusReady;
	}

	void DiskDevice::blockWrite(Address ramAddress, u32 size)
	{
		if (size == 0)
			return;

		const usize offset = static_cast<usize>(_sector) * SectorSize;
		if (offset >= _image.size() || size > SectorSize)
		{
			_status = StatusError;
			return;
		}

		const u32 available = static_cast<u32>(std::min<usize>(size, _image.size() - offset));
		const u32 clampSize = memory().clampBlockSize(ramAddress, available);
		if (clampSize == 0)
		{
			_status = StatusError;
			return;
		}

		const auto bytes = memory().peekBytes(ramAddress, clampSize);
		std::copy_n(bytes.begin(), clampSize, _image.begin() + static_cast<std::ptrdiff_t>(offset));
		_status = StatusReady;
		_dirty = true;
	}

	u32 DiskDevice::read(Address offset)
	{
		if (offset == StatusRegister) return _status;
		if (offset == SectorRegister) return _sector;
		if (offset == SectorCountRegister) return sectorCount();
		return 0;
	}

	void DiskDevice::write(Address offset, u32 value)
	{
		if (offset == SectorRegister)
		{
			_sector = value;
			_status = value < sectorCount() ? StatusReady : StatusError;
			return;
		}
		if (offset == CommandRegister)
		{
			if (value == CommandFlush)
				_status = flush() ? StatusReady : StatusError;
			return;
		}
		if (offset == BlockAddressRegister) { _blockAddress = value; return; }
		if (offset == BlockLengthRegister) { _blockLength = value; return; }
		if (offset == BlockCommandRegister)
		{
			if (value == BlockCommandRead)
				blockRead(Address(_blockAddress), _blockLength);
			else if (value == BlockCommandWrite)
				blockWrite(Address(_blockAddress), _blockLength);
		}
	}

	const RegisterMap& DiskDevice::registers() const
	{
		static constexpr RegisterMap map{ "disk", Registers };
		return map;
	}
}

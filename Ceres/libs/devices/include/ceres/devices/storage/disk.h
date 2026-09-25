#pragma once

// The disk: fixed-size sectors, backed by a host file or by memory.

#include <ceres/vm/mmio_bus.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ceres::devices
{
	using namespace vm;

	// A disk of fixed-size sectors, backed by a host file when it is given one and by memory
	// otherwise. A transfer goes through the block registers: the sector number says which one,
	// and BLOCK_ADDR/BLOCK_LEN/BLOCK_CMD say where in memory it goes.
	//
	//   li   r1, 3
	//   str  [r_sector + 0], r1     // which sector
	//   str  [r_disk + BLOCK_ADDR], r_buffer
	//   str  [r_disk + BLOCK_LEN], r2      // 512
	//   li   r3, 1
	//   str  [r_disk + BLOCK_CMD], r3       // 1 = read sector -> buffer
	//   ldrb r4, [r_status]
	class DiskDevice final : public IODevice
	{
	public:
		static inline constexpr Address StatusRegister = Address(0x00);
		static inline constexpr Address CommandRegister = Address(0x04);
		static inline constexpr Address SectorRegister = Address(0x08);
		static inline constexpr Address SectorCountRegister = Address(0x0C); // Read-only: how many sectors the disk has.
		static inline constexpr Address BlockAddressRegister = Address(0xF0);
		static inline constexpr Address BlockLengthRegister = Address(0xF4);
		static inline constexpr Address BlockCommandRegister = Address(0xF8);

		static inline constexpr u32 SectorSize = 512;

		// Status bits. Ready is set unless the last operation failed, so a program that never
		// checks still behaves, and one that does gets told.
		static inline constexpr u32 StatusReady = 1u << 0;
		static inline constexpr u32 StatusError = 1u << 1;

		// Commands, written to the command register. A flush is only meaningful for a file-backed
		// disk; on a memory one it succeeds and does nothing.
		static inline constexpr u32 CommandFlush = 1;

		// Block commands: which direction BLOCK_CMD moves the selected sector.
		static inline constexpr u32 BlockCommandRead = 1;  // sector -> RAM
		static inline constexpr u32 BlockCommandWrite = 2; // RAM -> sector

	private:
		std::vector<u8> _image;
		std::filesystem::path _path; // Empty for a disk that lives only in memory
		u32 _sector = 0;
		u32 _status = StatusReady;
		bool _dirty = false;
		u32 _blockAddress = 0;
		u32 _blockLength = 0;

	public:
		// A disk of `sectors` empty sectors, with nothing behind it.
		explicit DiskDevice(u32 sectors = 64) :
			_image(static_cast<usize>(sectors) * SectorSize, 0)
		{}

		DiskDevice(const DiskDevice&) = delete;
		DiskDevice(DiskDevice&&) = delete;
		~DiskDevice() override;

		DiskDevice& operator=(const DiskDevice&) = delete;
		DiskDevice& operator=(DiskDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Disk, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Disk);
		}

		// Backs the disk with a host file, creating it at `sectors` sectors if it is not there.
		// Returns false when the file cannot be read, and leaves the disk as it was.
		bool open(const std::filesystem::path& path, u32 sectors = 64);

		bool flush();

		u32 sectorCount() const noexcept { return static_cast<u32>(_image.size() / SectorSize); }
		std::span<const u8> image() const noexcept { return _image; }

	private:
		// `size` bytes of the selected sector, into memory at `ramAddress`.
		void blockRead(Address ramAddress, u32 size);

		// `size` bytes of memory at `ramAddress`, into the selected sector.
		void blockWrite(Address ramAddress, u32 size);

	public:
		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedWord(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedWord(offset)); }

		u32 readUnsignedWord(Address offset) override;

		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }

		void writeWord(Address offset, u32 value) override;
	};
}

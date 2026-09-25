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
		~PeripheralDevice() override;

		PeripheralDevice& operator=(const PeripheralDevice&) = delete;
		PeripheralDevice& operator=(PeripheralDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus) { bus.attach(default_mmio::Peripherals, *this); }
		void detachFrom(MmioBus& bus) { bus.detach(default_mmio::Peripherals); }

		// ---- the host's side --------------------------------------------------------------------------------------

		// Plugs a medium with these contents into `port`. False when there is no such port or something is already in
		// it: a second connection is refused, not replaced. A storage medium is padded to a whole number of sectors;
		// a cartridge is always write protected.
		bool attachImage(u32 port, std::string name, std::vector<u8> image, Kind kind, std::filesystem::path backingFile = {}, bool writeProtected = false);

		// Plugs in the host file at `path`. A storage medium whose file is not there yet is created with 64 sectors,
		// as the disk does; a cartridge has to exist and hold something. False, with `error` saying why, when that
		// cannot be done.
		bool attachFile(u32 port, const std::filesystem::path& path, Kind kind, std::string* error = nullptr, bool writeProtected = false);

		// Plugs a file into the first port that is free, and says which; -1 when they are all taken or it will not open.
		int attachToFreePort(const std::filesystem::path& path, Kind kind, std::string* error = nullptr);

		// Pulls the medium out of `port`, keeping what was written to it. False when there was nothing there.
		bool detach(u32 port);

		void setWriteProtected(u32 port, bool protectedMedium);

		bool isPresent(u32 port) const;

		u32 sectorCount(u32 port) const;

		// A copy of the medium's bytes, for a test or a debugger to look at.
		std::vector<u8> imageOf(u32 port) const;

		usize pendingEvents() const;

		// What the port holds, in words: "empty", "storage stick.img (64 sectors)".
		std::string describe(u32 port) const;

	private:
		static bool flushPort(Port& port);

		// FNV-1a of the medium's name, mixed with its size: the same file is the same identifier every time, and two
		// different ones almost never share it.
		static u32 identifierOf(const Port& port);

		u32 statusLocked() const
		{
			return (_events.empty() ? 0u : StatusEventPending) | (_error ? StatusError : StatusReady);
		}

		// `size` bytes of the selected medium's selected sector, to or from RAM at `ramAddress`. A transfer that runs
		// off the end of the sector or of RAM is clamped, as with the disk.
		bool transfer(bool toRam);

	public:
		// ---- the program's side -----------------------------------------------------------------------------------

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

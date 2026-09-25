#pragma once

// Files of the host, reached from a program: semihosting. `ceres run --host-dir <dir>` gives the machine
// one directory of the host, and a program opens, reads, writes, lists and removes files under it by name -
// levels, saves, logs, test data - with no disk image in between. Nothing outside that directory can be
// named: a path is relative, and a component that is empty, "." or "..", or carries a drive or a root, is
// refused. Without --host-dir the device is there but every operation fails with ENODEV.
//
// An operation is a write to the command register, carried out at once; its outcome is in the result
// register: 0 or more on success (a handle, a byte count, a position, a size), minus an errno number on
// failure (the C library's numbering: ENOENT 2, EBADF 9, EACCES 13, EEXIST 17, ENODEV 19, ENOTDIR 20,
// EISDIR 21, EINVAL 22, EMFILE 24, ENAMETOOLONG 36, ENOTEMPTY 39, EIO 5).
//
//   str [r_host + ADDRESS], r_path          // "levels/1.txt", NUL-terminated
//   li  r1, 1                               // read
//   str [r_host + ARGUMENT], r1
//   li  r1, 1                               // open
//   str [r_host + COMMAND], r1
//   ldr r2, [r_host + RESULT]               // the handle, or -ENOENT

#include <ceres/vm/mmio_bus.h>
#include <array>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace ceres::devices
{
	using namespace vm;

	class HostFsDevice final : public IODevice
	{
	public:
		static inline constexpr Address StatusRegister = Address(0x00);   // Read: bit 0 a host directory is attached
		static inline constexpr Address CommandRegister = Address(0x04);  // Write: the operation, carried out at once
		static inline constexpr Address HandleRegister = Address(0x08);   // Read/write: the open file an operation acts on
		static inline constexpr Address AddressRegister = Address(0x0C);  // Write: RAM address of a path (NUL-terminated) or of data
		static inline constexpr Address LengthRegister = Address(0x10);   // Write: bytes to move, or the size of a buffer
		static inline constexpr Address ArgumentRegister = Address(0x14); // Write: open flags, seek whence, an entry index, a second path
		static inline constexpr Address OffsetRegister = Address(0x18);   // Write: a seek offset (signed), or where a listed name goes
		static inline constexpr Address ResultRegister = Address(0x1C);   // Read: what the last operation gave, or minus an errno

		static inline constexpr u32 StatusAttached = 1u << 0;

		// Commands.
		static inline constexpr u32 CommandOpen = 1;       // path at ADDRESS, flags in ARGUMENT -> handle
		static inline constexpr u32 CommandClose = 2;      // HANDLE -> 0
		static inline constexpr u32 CommandRead = 3;       // HANDLE, ADDRESS, LENGTH -> bytes read (0 at the end)
		static inline constexpr u32 CommandWrite = 4;      // HANDLE, ADDRESS, LENGTH -> bytes written
		static inline constexpr u32 CommandSeek = 5;       // HANDLE, OFFSET, whence in ARGUMENT (0 set, 1 current, 2 end) -> position
		static inline constexpr u32 CommandFileSize = 6;   // HANDLE -> size
		static inline constexpr u32 CommandRemove = 7;     // path at ADDRESS -> 0 (a file, or an empty directory)
		static inline constexpr u32 CommandRename = 8;     // old path at ADDRESS, new path at ARGUMENT -> 0
		static inline constexpr u32 CommandStat = 9;       // path at ADDRESS -> size of a file, -EISDIR for a directory
		static inline constexpr u32 CommandList = 10;      // directory at ADDRESS ("" the root), index in ARGUMENT, name buffer at
		                                                   // OFFSET of LENGTH bytes -> the name's length (0: no more); a directory's
		                                                   // name ends in '/'
		static inline constexpr u32 CommandMakeDirectory = 11;   // path at ADDRESS -> 0

		// Open flags.
		static inline constexpr u32 OpenRead = 1u << 0;
		static inline constexpr u32 OpenWrite = 1u << 1;
		static inline constexpr u32 OpenCreate = 1u << 2;
		static inline constexpr u32 OpenTruncate = 1u << 3;
		static inline constexpr u32 OpenAppend = 1u << 4;
		static inline constexpr u32 OpenExclusive = 1u << 5;   // with OpenCreate: fail if it is there

		static inline constexpr usize MaxOpenFiles = 8;
		static inline constexpr u32 MaxPathLength = 255;

		// The C library's errno numbers the result register uses.
		static inline constexpr i32 ErrNoEntry = 2, ErrIo = 5, ErrBadHandle = 9, ErrAccess = 13, ErrExists = 17,
			ErrNoDevice = 19, ErrNotDirectory = 20, ErrIsDirectory = 21, ErrInvalid = 22, ErrTooManyOpen = 24,
			ErrNameTooLong = 36, ErrNotEmpty = 39;

	private:
		struct OpenFile
		{
			std::fstream stream;
			bool append = false;
		};

		std::filesystem::path _root;              // empty: none attached
		std::array<std::unique_ptr<OpenFile>, MaxOpenFiles> _files{};
		u32 _handle = 0;
		u32 _address = 0;
		u32 _length = 0;
		u32 _argument = 0;
		u32 _offset = 0;
		i32 _result = 0;

	public:
		HostFsDevice() = default;
		HostFsDevice(const HostFsDevice&) = delete;
		HostFsDevice(HostFsDevice&&) = delete;
		~HostFsDevice() override = default;

		HostFsDevice& operator=(const HostFsDevice&) = delete;
		HostFsDevice& operator=(HostFsDevice&&) = delete;

		void attachTo(MmioBus& bus) { bus.attach(default_mmio::HostFs, *this); }
		void detachFrom(MmioBus& bus) { bus.detach(default_mmio::HostFs); }

		// The directory the program may reach, which has to exist. False (and nothing attached) when it does not.
		bool setRoot(const std::filesystem::path& directory);

		const std::filesystem::path& root() const noexcept { return _root; }

		// A reset closes every file the program had open: the fresh program starts with none.
		void reset() override;

		// The host path a program's name stands for, or an errno when the name may not be used: it must be
		// relative, and every component a plain name. An empty name is the root itself.
		std::expected<std::filesystem::path, i32> resolve(std::string_view name) const;

	private:
		// A NUL-terminated name from RAM, at most MaxPathLength bytes; empty and an error when it runs past that
		// or out of RAM.
		std::expected<std::string, i32> readName(u32 address) const;

		static i32 errnoOf(const std::error_code& error);

		OpenFile* fileAt(u32 handle)
		{
			return handle < MaxOpenFiles ? _files[handle].get() : nullptr;
		}

		i32 open();

		i32 readFile();

		i32 writeFile();

		i32 seek();

		i32 fileSize();

		i32 remove();

		i32 rename();

		i32 stat();

		// The entries of a directory, sorted by name so an index means the same thing every time.
		i32 list();

		i32 makeDirectory();

		void command(u32 value);

	public:
		u32 read(Address offset) override;
		void write(Address offset, u32 value) override;
		const RegisterMap& registers() const override;
	};
}

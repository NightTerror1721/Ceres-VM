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
#include <algorithm>
#include <array>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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
		bool setRoot(const std::filesystem::path& directory)
		{
			std::error_code error;
			if (!std::filesystem::is_directory(directory, error))
				return false;
			_root = std::filesystem::absolute(directory, error);
			return !error;
		}

		const std::filesystem::path& root() const noexcept { return _root; }

		// A reset closes every file the program had open: the fresh program starts with none.
		void reset() override
		{
			for (auto& file : _files)
				file.reset();
			_result = 0;
		}

		// The host path a program's name stands for, or an errno when the name may not be used: it must be
		// relative, and every component a plain name. An empty name is the root itself.
		std::expected<std::filesystem::path, i32> resolve(std::string_view name) const
		{
			if (_root.empty())
				return std::unexpected(ErrNoDevice);
			if (name.size() > MaxPathLength)
				return std::unexpected(ErrNameTooLong);
			std::filesystem::path result = _root;
			std::filesystem::path relative;
			usize start = 0;
			while (start < name.size())                    // "" is the root; one trailing slash is allowed
			{
				usize end = name.find_first_of("/\\", start);
				if (end == std::string_view::npos)
					end = name.size();
				const std::string_view part = name.substr(start, end - start);
				if (part.empty() || part == "." || part == ".." || part.find(':') != std::string_view::npos)
					return std::unexpected(ErrInvalid);    // a leading slash, "a//b", a step up, a drive
				result /= std::filesystem::path(std::u8string(part.begin(), part.end()));
				start = end + 1;
			}
			// A link inside the directory must not lead out of it: what the name resolves to, following links,
			// has to be under the root's own resolution too.
			std::error_code error;
			const std::filesystem::path real = std::filesystem::weakly_canonical(result, error);
			const std::filesystem::path realRoot = std::filesystem::weakly_canonical(_root, error);
			if (error)
				return std::unexpected(ErrAccess);
			auto [rootEnd, realAt] = std::mismatch(realRoot.begin(), realRoot.end(), real.begin(), real.end());
			if (rootEnd != realRoot.end())
				return std::unexpected(ErrAccess);
			return result;
		}

	private:
		// A NUL-terminated name from RAM, at most MaxPathLength bytes; empty and an error when it runs past that
		// or out of RAM.
		std::expected<std::string, i32> readName(u32 address) const
		{
			const u32 available = memory().clampBlockSize(Address(address), MaxPathLength + 1);
			if (available == 0)
				return std::unexpected(ErrInvalid);
			const auto bytes = memory().peekBytes(Address(address), available);
			for (u32 i = 0; i < bytes.size(); ++i)
				if (bytes[i] == 0)
					return std::string(reinterpret_cast<const char*>(bytes.data()), i);
			// No NUL: past the limit, or the name runs off the end of RAM - a bad pointer, not a long name.
			return std::unexpected(available == MaxPathLength + 1 ? ErrNameTooLong : ErrInvalid);
		}

		static i32 errnoOf(const std::error_code& error)
		{
			if (error == std::errc::no_such_file_or_directory) return ErrNoEntry;
			if (error == std::errc::file_exists) return ErrExists;
			if (error == std::errc::permission_denied) return ErrAccess;
			if (error == std::errc::not_a_directory) return ErrNotDirectory;
			if (error == std::errc::is_a_directory) return ErrIsDirectory;
			if (error == std::errc::directory_not_empty) return ErrNotEmpty;
			if (error == std::errc::filename_too_long) return ErrNameTooLong;
			return ErrIo;
		}

		OpenFile* fileAt(u32 handle)
		{
			return handle < MaxOpenFiles ? _files[handle].get() : nullptr;
		}

		i32 open()
		{
			auto name = readName(_address);
			if (!name) return -name.error();
			auto path = resolve(*name);
			if (!path) return -path.error();
			const u32 flags = _argument;
			if ((flags & (OpenRead | OpenWrite)) == 0)
				return -ErrInvalid;
			usize slot = MaxOpenFiles;
			for (usize i = 0; i < MaxOpenFiles && slot == MaxOpenFiles; ++i)
				if (!_files[i])
					slot = i;
			if (slot == MaxOpenFiles)
				return -ErrTooManyOpen;

			std::error_code error;
			const bool exists = std::filesystem::exists(*path, error);
			if (exists && std::filesystem::is_directory(*path, error))
				return -ErrIsDirectory;
			if (exists && (flags & OpenCreate) && (flags & OpenExclusive))
				return -ErrExists;
			if (!exists)
			{
				if (!(flags & OpenCreate) || !(flags & OpenWrite))
					return -ErrNoEntry;
				if (!std::filesystem::is_directory(path->parent_path(), error))
					return -ErrNoEntry;                    // as open(O_CREAT) says for a missing directory
			}
			if (!exists || ((flags & OpenWrite) && (flags & OpenTruncate)))
			{
				std::ofstream create(*path, std::ios::binary | std::ios::trunc);   // empty, so in|out can open it
				if (!create)
					return -ErrAccess;
			}
			// Always in|out for a writer (out alone truncates, and has no read position to seek or measure by).
			std::ios::openmode mode = std::ios::binary | std::ios::in;
			if (flags & OpenWrite) mode |= std::ios::out;
			auto file = std::make_unique<OpenFile>();
			file->stream.open(*path, mode);
			if (!file->stream)
				return -ErrAccess;
			file->append = (flags & OpenAppend) != 0;
			_files[slot] = std::move(file);
			return static_cast<i32>(slot);
		}

		i32 read()
		{
			OpenFile* file = fileAt(_handle);
			if (!file) return -ErrBadHandle;
			const u32 size = memory().clampBlockSize(Address(_address), _length);
			if (size == 0) return 0;
			auto buffer = memory().peekMutBytes(Address(_address), size);
			file->stream.clear();
			file->stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
			const std::streamsize got = file->stream.gcount();
			file->stream.clear();                      // a short read at the end is not an error
			return static_cast<i32>(got);
		}

		i32 write()
		{
			OpenFile* file = fileAt(_handle);
			if (!file) return -ErrBadHandle;
			const u32 size = memory().clampBlockSize(Address(_address), _length);
			if (size == 0) return 0;
			const auto buffer = memory().peekBytes(Address(_address), size);
			file->stream.clear();
			if (file->append)
				file->stream.seekp(0, std::ios::end);
			file->stream.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
			file->stream.flush();
			if (!file->stream)
			{
				file->stream.clear();
				return -ErrIo;
			}
			return static_cast<i32>(size);
		}

		i32 seek()
		{
			OpenFile* file = fileAt(_handle);
			if (!file) return -ErrBadHandle;
			const i32 offset = static_cast<i32>(_offset);
			std::ios::seekdir dir = _argument == 0 ? std::ios::beg : _argument == 1 ? std::ios::cur : std::ios::end;
			if (_argument > 2)
				return -ErrInvalid;
			file->stream.clear();
			const std::streamoff current = file->stream.tellg();
			file->stream.seekg(0, std::ios::end);
			const std::streamoff end = file->stream.tellg();
			const std::streamoff base = dir == std::ios::beg ? 0 : dir == std::ios::cur ? current : end;
			const std::streamoff target = base + offset;
			if (target < 0 || target > 0x7FFFFFFF)
			{
				file->stream.seekg(current);
				return -ErrInvalid;
			}
			file->stream.seekg(target);
			file->stream.seekp(target);
			return static_cast<i32>(target);
		}

		i32 fileSize()
		{
			OpenFile* file = fileAt(_handle);
			if (!file) return -ErrBadHandle;
			file->stream.clear();
			const std::streamoff current = file->stream.tellg();
			file->stream.seekg(0, std::ios::end);
			const std::streamoff end = file->stream.tellg();
			file->stream.seekg(current);
			return end > 0x7FFFFFFF ? -ErrIo : static_cast<i32>(end);
		}

		i32 remove()
		{
			auto name = readName(_address);
			if (!name) return -name.error();
			if (name->empty()) return -ErrInvalid;                 // not the root itself
			auto path = resolve(*name);
			if (!path) return -path.error();
			std::error_code error;
			if (!std::filesystem::exists(*path, error))
				return -ErrNoEntry;
			if (std::filesystem::is_directory(*path, error) && !std::filesystem::is_empty(*path, error))
				return -ErrNotEmpty;                       // said the same on every host
			if (!std::filesystem::remove(*path, error))
				return -errnoOf(error);
			return 0;
		}

		i32 rename()
		{
			auto from = readName(_address);
			if (!from) return -from.error();
			auto to = readName(_argument);
			if (!to) return -to.error();
			if (from->empty() || to->empty()) return -ErrInvalid;
			auto fromPath = resolve(*from);
			if (!fromPath) return -fromPath.error();
			auto toPath = resolve(*to);
			if (!toPath) return -toPath.error();
			std::error_code error;
			if (!std::filesystem::exists(*fromPath, error))
				return -ErrNoEntry;
			std::filesystem::rename(*fromPath, *toPath, error);
			return error ? -errnoOf(error) : 0;
		}

		i32 stat()
		{
			auto name = readName(_address);
			if (!name) return -name.error();
			auto path = resolve(*name);
			if (!path) return -path.error();
			std::error_code error;
			const auto status = std::filesystem::status(*path, error);
			if (error || !std::filesystem::exists(status))
				return -ErrNoEntry;
			if (std::filesystem::is_directory(status))
				return -ErrIsDirectory;
			const auto size = std::filesystem::file_size(*path, error);
			if (error) return -errnoOf(error);
			return size > 0x7FFFFFFF ? -ErrIo : static_cast<i32>(size);
		}

		// The entries of a directory, sorted by name so an index means the same thing every time.
		i32 list()
		{
			auto name = readName(_address);
			if (!name) return -name.error();
			auto path = resolve(*name);
			if (!path) return -path.error();
			std::error_code error;
			if (!std::filesystem::is_directory(*path, error))
				return std::filesystem::exists(*path, error) ? -ErrNotDirectory : -ErrNoEntry;
			std::vector<std::string> names;
			for (const auto& entry : std::filesystem::directory_iterator(*path, error))
			{
				const std::u8string utf8 = entry.path().filename().u8string();
				std::string text(utf8.begin(), utf8.end());
				if (entry.is_directory(error))
					text += '/';
				names.push_back(std::move(text));
			}
			if (error) return -errnoOf(error);
			std::ranges::sort(names);
			if (_argument >= names.size())
				return 0;
			const std::string& entry = names[_argument];
			const u32 room = memory().clampBlockSize(Address(_offset), _length);
			if (room < entry.size() + 1)
				return -ErrNameTooLong;
			auto buffer = memory().peekMutBytes(Address(_offset), static_cast<u32>(entry.size() + 1));
			std::copy(entry.begin(), entry.end(), buffer.begin());
			buffer[entry.size()] = 0;
			return static_cast<i32>(entry.size());
		}

		i32 makeDirectory()
		{
			auto name = readName(_address);
			if (!name) return -name.error();
			if (name->empty()) return -ErrExists;
			auto path = resolve(*name);
			if (!path) return -path.error();
			std::error_code error;
			if (std::filesystem::exists(*path, error))
				return -ErrExists;
			if (!std::filesystem::create_directory(*path, error))
				return -errnoOf(error);
			return 0;
		}

		void command(u32 value)
		{
			if (_root.empty())
			{
				_result = -ErrNoDevice;
				return;
			}
			switch (value)
			{
			case CommandOpen: _result = open(); break;
			case CommandClose:
				if (OpenFile* file = fileAt(_handle); file)
				{
					_files[_handle].reset();
					_result = 0;
				}
				else
				{
					_result = -ErrBadHandle;
				}
				break;
			case CommandRead: _result = read(); break;
			case CommandWrite: _result = write(); break;
			case CommandSeek: _result = seek(); break;
			case CommandFileSize: _result = fileSize(); break;
			case CommandRemove: _result = remove(); break;
			case CommandRename: _result = rename(); break;
			case CommandStat: _result = stat(); break;
			case CommandList: _result = list(); break;
			case CommandMakeDirectory: _result = makeDirectory(); break;
			default: _result = -ErrInvalid; break;
			}
		}

	public:
		u32 readUnsignedWord(Address offset) override
		{
			if (offset == StatusRegister) return _root.empty() ? 0u : StatusAttached;
			if (offset == HandleRegister) return _handle;
			if (offset == ResultRegister) return static_cast<u32>(_result);
			return 0xFFFFFFFF;
		}
		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		void writeWord(Address offset, u32 value) override
		{
			if (offset == CommandRegister) command(value);
			else if (offset == HandleRegister) _handle = value;
			else if (offset == AddressRegister) _address = value;
			else if (offset == LengthRegister) _length = value;
			else if (offset == ArgumentRegister) _argument = value;
			else if (offset == OffsetRegister) _offset = value;
		}
		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }
	};
}

#include <ceres/devices/storage/host_fs.h>
#include <algorithm>
#include <vector>

namespace ceres::devices
{
	namespace
	{
		// Every register of the device (plan/v2 SPEC 5.3), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x00, "Status",   RegisterAccess::Read,      0x0, false, "Bit 0 a host directory is attached." },
			{ 0x04, "Command",  RegisterAccess::Write,     0x0, false, "The operation, carried out at once." },
			{ 0x08, "Handle",   RegisterAccess::ReadWrite, 0x0, false, "The open file an operation acts on." },
			{ 0x0C, "Address",  RegisterAccess::Write,     0x0, false, "RAM address of a path (NUL-terminated) or of data." },
			{ 0x10, "Length",   RegisterAccess::Write,     0x0, false, "Bytes to move, or the size of a buffer." },
			{ 0x14, "Argument", RegisterAccess::Write,     0x0, false, "Open flags, seek whence, an entry index, a second path." },
			{ 0x18, "Offset",   RegisterAccess::Write,     0x0, false, "A seek offset (signed), or where a listed name goes." },
			{ 0x1C, "Result",   RegisterAccess::Read,      0x0, false, "What the last operation gave, or minus an errno." },
		};
	}

	bool HostFsDevice::setRoot(const std::filesystem::path& directory)
	{
		std::error_code error;
		if (!std::filesystem::is_directory(directory, error))
			return false;
		_root = std::filesystem::absolute(directory, error);
		return !error;
	}

	void HostFsDevice::reset()
	{
		for (auto& file : _files)
			file.reset();
		_result = 0;
	}

	std::expected<std::filesystem::path, i32> HostFsDevice::resolve(std::string_view name) const
	{
		if (_root.empty())
			return std::unexpected(ErrNoDevice);
		if (name.size() > MaxPathLength)
			return std::unexpected(ErrNameTooLong);
		std::filesystem::path result = _root;
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
		const auto rootEnd = std::mismatch(realRoot.begin(), realRoot.end(), real.begin(), real.end()).first;
		if (rootEnd != realRoot.end())
			return std::unexpected(ErrAccess);
		return result;
	}

	std::expected<std::string, i32> HostFsDevice::readName(u32 address) const
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

	i32 HostFsDevice::errnoOf(const std::error_code& error)
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

	i32 HostFsDevice::open()
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
		if (error)
			return -errnoOf(error);               // a real failure, not "it is not there"
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

	i32 HostFsDevice::readFile()
	{
		OpenFile* file = fileAt(_handle);
		if (!file) return -ErrBadHandle;
		const u32 size = memory().clampBlockSize(Address(_address), _length);
		if (size == 0) return 0;
		auto buffer = memory().peekMutBytes(Address(_address), size);
		file->stream.clear();
		file->stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
		const bool failed = file->stream.bad();
		const std::streamsize got = file->stream.gcount();
		file->stream.clear();                      // a short read at the end is not an error
		if (failed)
			return -ErrIo;
		// One position for both directions, as a POSIX descriptor has: a write that follows starts here.
		file->stream.seekp(file->stream.tellg());
		return static_cast<i32>(got);
	}

	i32 HostFsDevice::writeFile()
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
		file->stream.seekg(file->stream.tellp());   // a read that follows continues from here
		return static_cast<i32>(size);
	}

	i32 HostFsDevice::seek()
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

	i32 HostFsDevice::fileSize()
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

	i32 HostFsDevice::remove()
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

	i32 HostFsDevice::rename()
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

	i32 HostFsDevice::stat()
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

	i32 HostFsDevice::list()
	{
		auto name = readName(_address);
		if (!name) return -name.error();
		auto path = resolve(*name);
		if (!path) return -path.error();
		std::error_code error;
		if (!std::filesystem::is_directory(*path, error))
			return std::filesystem::exists(*path, error) ? -ErrNotDirectory : -ErrNoEntry;
		std::vector<std::string> names;
		// Advanced with increment(error): the ++ of a range for throws on a failure part-way through.
		for (std::filesystem::directory_iterator it(*path, error), end; !error && it != end; it.increment(error))
		{
			const std::filesystem::directory_entry& entry = *it;
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

	i32 HostFsDevice::makeDirectory()
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

	void HostFsDevice::command(u32 value)
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
		case CommandRead: _result = readFile(); break;
		case CommandWrite: _result = writeFile(); break;
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

	u32 HostFsDevice::read(Address offset)
	{
		if (offset == StatusRegister) return _root.empty() ? 0u : StatusAttached;
		if (offset == HandleRegister) return _handle;
		if (offset == ResultRegister) return static_cast<u32>(_result);
		return 0xFFFFFFFF;
	}

	void HostFsDevice::write(Address offset, u32 value)
	{
		if (offset == CommandRegister) command(value);
		else if (offset == HandleRegister) _handle = value;
		else if (offset == AddressRegister) _address = value;
		else if (offset == LengthRegister) _length = value;
		else if (offset == ArgumentRegister) _argument = value;
		else if (offset == OffsetRegister) _offset = value;
	}

	const RegisterMap& HostFsDevice::registers() const
	{
		static constexpr RegisterMap map{ "host-fs", Registers };
		return map;
	}
}

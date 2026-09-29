#include <ceres/driver/driver.h>

#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#elif defined(__APPLE__)
#	include <mach-o/dyld.h>
#	include <cstdint>
#	include <vector>
#endif

namespace ceres::driver
{
	namespace
	{
		// The running ceres executable, or empty when the system does not say.
		std::filesystem::path executablePath()
		{
#if defined(_WIN32)
			std::wstring buffer(MAX_PATH, L'\0');
			for (;;)
			{
				const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
				if (length == 0)
					return {};
				if (length < buffer.size())
				{
					buffer.resize(length);
					return std::filesystem::path(buffer);
				}
				buffer.resize(buffer.size() * 2);
			}
#elif defined(__APPLE__)
			std::uint32_t size = 0;
			_NSGetExecutablePath(nullptr, &size);
			std::vector<char> buffer(size + 1, '\0');
			if (_NSGetExecutablePath(buffer.data(), &size) != 0)
				return {};
			std::error_code error;
			const auto canonical = std::filesystem::canonical(buffer.data(), error);
			return error ? std::filesystem::path(buffer.data()) : canonical;
#else
			std::error_code error;
			const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
			return error ? std::filesystem::path{} : path;
#endif
		}
	}

	std::vector<std::filesystem::path> installDirectories()
	{
		std::vector<std::filesystem::path> directories;
		// CERES_PATH names the directory, but a path to a file in it (ceres itself, say) is taken as that directory, as
		// ceresc takes it.
		if (const char* value = std::getenv("CERES_PATH"); value != nullptr && *value != '\0')
		{
			std::filesystem::path path = value;
			std::error_code error;
			if (std::filesystem::is_regular_file(path, error))
				path = path.parent_path();
			directories.push_back(std::move(path));
		}
		if (const auto executable = executablePath(); !executable.empty())
			directories.push_back(executable.parent_path());
		return directories;
	}
}

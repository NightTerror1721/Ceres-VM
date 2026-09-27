#include <ceres/driver/host_log.h>

#include <ceres/devices/system/debug_log.h>

namespace ceres::driver
{
	bool HostLog::openFile(const std::filesystem::path& path)
	{
		const std::lock_guard lock{ _mutex };
		_file.open(path, std::ios::binary | std::ios::trunc);
		if (!_file)
			return false;
		_stream = &_file;
		return true;
	}

	void HostLog::write(u32 level, std::string_view line)
	{
		const std::lock_guard lock{ _mutex };
		*_stream << "[ceres:" << devices::DebugLogDevice::levelName(level) << "] " << line << '\n';
		_stream->flush();
	}
}

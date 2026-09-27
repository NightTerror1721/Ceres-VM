#pragma once

#include <ceres/core/base/types.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <ostream>
#include <string_view>

namespace ceres::driver
{
	// The host's log (plan/v2 SPEC 5.7): what the machine tells whoever runs it, one line at a time as
	// `[ceres:<level>] <line>` - the program's debug log (DebugLogDevice) and the host's own diagnostics about
	// the run, such as an exception nobody handled. It goes to the host's stderr, or to the file of `--log`;
	// never to the program's terminal.
	class HostLog
	{
	public:
		static inline constexpr u32 Error = 0;
		static inline constexpr u32 Warning = 1;
		static inline constexpr u32 Info = 2;
		static inline constexpr u32 Debug = 3;

		explicit HostLog(std::ostream& stream) noexcept : _stream(&stream) {}
		HostLog(const HostLog&) = delete;
		HostLog& operator=(const HostLog&) = delete;

		// Sends every line to `path` from now on (created, or emptied). False when it cannot be written.
		bool openFile(const std::filesystem::path& path);

		// One line, without its newline. Called from the machine's thread and the host's alike.
		void write(u32 level, std::string_view line);
		void error(std::string_view line) { write(Error, line); }

	private:
		std::mutex _mutex;
		std::ostream* _stream;
		std::ofstream _file;
	};
}

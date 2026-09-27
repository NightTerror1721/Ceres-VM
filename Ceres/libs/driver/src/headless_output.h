#pragma once

// What a run leaves in files instead of the host's terminal (plan/v2 SPEC 10): the program never writes to the host's
// stdout, so a script or a test that wants its output asks for it here.
//
//   --transcript  every byte the program wrote to the terminal, in order; the error stream's between ESC [ E and
//                 ESC [ e, so the two can be told apart and still read in the order they were written.
//   --screen-log  the text plane as plain text (a line a row, UTF-8) at every Present, and once more at the end:
//                 each screen after a line "--- present <n> ---" or "--- end ---".

#include <ceres/core/base/types.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace ceres::driver
{
	class HeadlessOutput
	{
	public:
		static inline constexpr std::string_view ErrorStart = "\x1b[E";
		static inline constexpr std::string_view ErrorEnd = "\x1b[e";

	private:
		std::ofstream _transcript;
		std::ofstream _screenLog;
		bool _inError = false;
		u64 _screens = 0;

	public:
		// Opens what was asked for (an empty path is not asked for). The reason when a file cannot be written.
		std::string open(const std::filesystem::path& transcript, const std::filesystem::path& screenLog);

		bool hasTranscript() const noexcept { return _transcript.is_open(); }
		bool hasScreenLog() const noexcept { return _screenLog.is_open(); }

		// A byte the program wrote to its output, or to its error stream.
		void transcriptByte(u8 byte, bool error);

		// The screen as text, at a Present (`final` false) or at the end of the run.
		void screen(std::string_view text, bool final);

		// The run is over: an error stream left open is closed, and the files are flushed.
		void finish();
	};
}

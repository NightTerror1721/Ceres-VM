#pragma once

#include <ceres/driver/command.h>
#include <ceres/driver/machine.h>
#include <ceres/driver/audio_output.h>
#include <ceres/driver/host_input.h>
#include <ceres/driver/video_output.h>
#include <ceres/driver/pacer.h>
#include <ceres/core/format/debug_info.h>
#include <ceres/vm/ceresvm.h>
#include <iosfwd>

namespace ceres::driver
{
	struct HostServices;

	// How the machine itself is set up, apart from what is plugged into it.
	struct MachineOptions
	{
		bool strictMmio = false;          // --strict-mmio
		std::optional<i64> rtc;           // --rtc: the real-time clock's start, seconds since 1970
		std::optional<Speed> speed;       // --speed; unset, realtime while a window is open and max otherwise
		std::filesystem::path record;     // --record: the host's input, stamped with its cycles, written here
		std::filesystem::path replay;     // --replay: a recording fed back instead of the host's input
		std::filesystem::path logFile;    // --log: the host's log goes here instead of the diagnostics stream
		u32 refresh = 60;                 // --refresh: the display's frames a second, 50 or 60
		bool fullscreen = false;          // --fullscreen
		bool exitOnHalt = false;          // --exit-on-halt: the window closes when the program ends
		bool requireWindow = false;       // --window: a window that cannot be opened is an error
		std::filesystem::path framesDir;  // --frames: a PNG of the screen for every Present
		std::filesystem::path transcript; // --transcript: every byte the program wrote to the terminal
		std::filesystem::path screenLog;  // --screen-log: the text plane as text at every Present and at the end
		std::filesystem::path typeFile;   // --type: text typed on the terminal when the machine starts
		std::filesystem::path keysFile;   // --keys: keyboard events at instants of the machine's time
	};

	// The parts of a host the machine runs on; each may be missing (no window: all three are).
	struct HostIo
	{
		HostInput* input = nullptr;
		VideoOutput* video = nullptr;
		AudioOutput* audio = nullptr;

		bool windowed() const noexcept { return input != nullptr || video != nullptr; }
	};

	int runMachine(const fmt::Program& program, const MachineProfile& machine, const fmt::DebugInfo* profileInfo,
		const std::filesystem::path& diskImage, const std::vector<PortAttachment>& ports, vm::ProgramArguments arguments,
		const std::filesystem::path& hostDirectory, HostServices services, HostIo host = {},
		const MachineOptions& options = {});
}

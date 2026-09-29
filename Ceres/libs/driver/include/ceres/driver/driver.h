#pragma once

#include "command.h"
#include "audio_output.h"
#include "host_input.h"
#include "video_output.h"
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <vector>

namespace ceres::driver
{
	// Host-owned streams keep the command adapter usable by terminals and tests. Native GUI hosts
	// should invoke execute() with their own stream adapters or use the lower-level VM libraries.
	struct HostServices
	{
		// The host's standard input. A run never reads it (plan/v2 SPEC 1.4): the program's input is its terminal's,
		// typed in the window or given with --type and --keys.
		std::istream* input = nullptr;
		std::ostream* output = nullptr;
		std::ostream* diagnostics = nullptr;
		// Where Ceres is installed, in the order they are tried: the shell (plan/v2 F7) is shell/shell.cres in the
		// first that has one. The command line passes installDirectories(); a run given none has no shell.
		std::vector<std::filesystem::path> installDirectories;
	};

	// The directory CERES_PATH names, when it is set, and then the one the running ceres executable is in.
	std::vector<std::filesystem::path> installDirectories();

	// A windowed host, in its three parts: its input, its screen and its speakers. Any of them may be missing (a
	// host without a sound card has no audio). Shared, because one object may play more than one part - the SDL
	// window is both the input and what the presenter draws on - and each part keeps alive what it needs.
	struct WindowHost
	{
		std::shared_ptr<HostInput> input;
		std::shared_ptr<VideoOutput> video;
		std::shared_ptr<AudioOutput> audio;

		bool empty() const noexcept { return !input && !video && !audio; }
	};

	// The driver never links SDL itself. A windowed host (the CLI) registers a factory that the
	// driver calls when `run --window` is asked for; empty (the default) means "built without a
	// windowed host", and `--window` then reports an error instead of doing nothing. The factory throws
	// std::runtime_error when it cannot make one.
	using WindowHostFactory = std::function<WindowHost()>;

	int execute(const Command& command, HostServices services, WindowHostFactory windowHost = {});
	int runCommandLine(int argc, char* const argv[], HostServices services, WindowHostFactory windowHost = {});
}

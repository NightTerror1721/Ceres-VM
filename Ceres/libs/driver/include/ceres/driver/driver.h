#pragma once

#include "command.h"
#include "audio_output.h"
#include "host_input.h"
#include "video_output.h"
#include <functional>
#include <iosfwd>
#include <memory>

namespace ceres::driver
{
	// Host-owned streams keep the command adapter usable by terminals and tests. Native GUI hosts
	// should invoke execute() with their own stream adapters or use the lower-level VM libraries.
	struct HostServices
	{
		// The program's standard input. &std::cin is read on a thread of its own that may outlive the run.
		// Any other stream is read on a thread that the run waits for before it returns, so it must come to
		// an end on its own - a string or a file, not a pipe or a socket that can block forever.
		std::istream* input = nullptr;
		std::ostream* output = nullptr;
		std::ostream* diagnostics = nullptr;
	};

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

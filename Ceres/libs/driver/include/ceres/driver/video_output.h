#pragma once

// The host's screen: where the GPU's frames are shown. A host without one (headless) is no VideoOutput at all; a
// windowed host (libs/sdl) implements it. See host_input.h for the input side.
//
// The machine hands it a frame at every vertical blank (plan/v2 F5.5), composed by the GPU's executor, and tells it
// what to show in its status bar: the profile, how fast the machine is running and, once the program is done, how it
// ended. The window stays open after the program ends until the user closes it or presses a key (plan/v2 D22).

#include <ceres/devices/video/gpu_executor.h>

#include <optional>
#include <string>

namespace ceres::driver
{
	// What the window's status bar says.
	struct HostStatus
	{
		std::string profile;              // the machine's profile, by name
		double speed = 0.0;               // the machine's seconds per host second, as the pacer measures it
		std::optional<int> exitCode;      // set once the program has ended
	};

	class VideoOutput
	{
	public:
		virtual ~VideoOutput() = default;

		// Opens the window, sized for a screen of width x height. False if it cannot be opened (no display).
		virtual bool openWindow(u32 width, u32 height) = 0;

		// Show a frame. The window follows its size.
		virtual void present(const devices::video::VideoFrame& frame) = 0;

		// Whether the window is open now. A machine paces itself in real time while it is (plan/v2 SPEC 3.3).
		virtual bool windowOpen() const noexcept { return false; }

		// Full screen or a window (--fullscreen; F11 toggles it from the window itself).
		virtual void setFullscreen(bool) {}

		// What the status bar shows.
		virtual void setStatus(const HostStatus&) {}
	};
}

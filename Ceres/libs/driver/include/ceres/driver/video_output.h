#pragma once

// The host's screen: where the machine's frames are shown. A host without one (the default) is no VideoOutput at
// all; a windowed host (libs/sdl) implements it. See host_input.h for the input side.

#include <ceres/devices/video/display.h>
#include <ceres/devices/video/text_framebuffer.h>

namespace ceres::driver
{
	class VideoOutput
	{
	public:
		virtual ~VideoOutput() = default;

		// Show the display's current pixels.
		virtual void present(const devices::DisplayDevice& display) = 0;

		// Does this host show the text framebuffer in a window? A host that says no (the default) leaves every
		// frame to the terminal. One that says yes is given the frames the program presents while its output is
		// the window, and opens its window when it gets the first.
		virtual bool showsText() const noexcept { return false; }

		// Show one frame of the text framebuffer. Returns false if it cannot - no display to open a window on -
		// and the machine then sends that frame and all later ones to the terminal instead.
		virtual bool presentText(const devices::FramebufferDevice::Frame&) { return false; }

		// Open the window now, rather than when the first frame comes. False if it cannot be opened.
		virtual bool openWindow() { return true; }

		// Whether the host's window is open now. Without --speed, a machine paces itself in real time while it is,
		// and runs flat out while it is not (plan/v2 SPEC 3.3).
		virtual bool windowOpen() const noexcept { return false; }

		// How fast the machine is running: its seconds per host second, measured by the pacer. A host with a
		// status bar shows it; the default ignores it.
		virtual void reportSpeed(double) {}
	};
}

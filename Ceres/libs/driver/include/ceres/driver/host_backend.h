#pragma once

// The seam between the machine and whatever host is showing it. A backend pumps host input into
// the devices and shows the display's pixels once per frame slice; the driver itself never links
// SDL, so a windowed host lives in a separate library (libs/sdl) and is handed in as a HostBackend.
// The default is HeadlessBackend, which pumps nothing and shows nothing.

#include <ceres/devices/input_devices.h>
#include <ceres/devices/display_device.h>
#include <ceres/devices/storage_devices.h>
#include <ceres/devices/audio_device.h>

namespace ceres::driver
{
	using namespace devices;

	class HostBackend
	{
	public:
		virtual ~HostBackend() = default;

		// Pump pending host input into the keyboard, mouse and gamepad. Returns false when the host
		// wants the machine to stop (for example, the window was closed).
		virtual bool pump(KeyboardDevice& keyboard, MouseDevice& mouse, GamepadDevice& gamepad) = 0;

		// Show the display's current pixels.
		virtual void present(const DisplayDevice& display) = 0;

		// Does this host show the text framebuffer in a window? A host that says no (the default) leaves every
		// frame to the terminal. One that says yes is given the frames the program presents while its output is
		// the window, and opens its window when it gets the first.
		virtual bool showsText() const noexcept { return false; }

		// Show one frame of the text framebuffer. Returns false if it cannot - no display to open a window on -
		// and the machine then sends that frame and all later ones to the terminal instead.
		virtual bool presentText(const FramebufferDevice::Frame&) { return false; }

		// Open the window now, rather than when the first frame comes. False if it cannot be opened.
		virtual bool openWindow() { return true; }

		// Give the host the audio device to play, by installing a sink on it. Called once before the
		// machine runs; a host without speakers ignores it, and the machine is silent.
		virtual void attachAudio(AudioDevice&) {}

		// The machine is done with the audio device: stop making sound and never touch it again.
		virtual void detachAudio() {}

		// How many instructions to execute between pump/present calls. A windowed host runs a few
		// thousand per frame; a headless one can simply run to completion without ever calling this.
		virtual u64 instructionsPerFrame() const noexcept { return 4096; }
	};

	// The default: no window, no input beyond the terminal's own stdin reader, nothing to present.
	// Pumping never asks the machine to stop.
	class HeadlessBackend final : public HostBackend
	{
	public:
		bool pump(KeyboardDevice&, MouseDevice&, GamepadDevice&) override { return true; }
		void present(const DisplayDevice&) override {}
	};
}

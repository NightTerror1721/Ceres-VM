#pragma once

// The host's input side: what a window collects (keys, text, the mouse, the gamepad, dropped files) and hands to the
// machine. The driver never links SDL; a windowed host lives in libs/sdl and is handed in as these interfaces
// (driver.h, WindowHost). Its other halves are video_output.h and audio_output.h, and the host's log is host_log.h.

#include <ceres/core/base/types.h>

#include <filesystem>
#include <functional>
#include <string_view>

namespace ceres::driver
{
	// Where a host's pump() puts the input it collected: the driver stamps it with the machine's cycle and hands
	// it to the devices between two slices (input_journal.h), so a run can be recorded and replayed.
	class InputSink
	{
	public:
		virtual ~InputSink() = default;
		virtual void key(u32 code, bool pressed) = 0;                    // a key went down or up (a scancode)
		virtual void text(std::string_view utf8) = 0;                    // text typed
		virtual void mouse(i32 dx, i32 dy, u8 buttons, i8 wheel) = 0;   // relative motion, the buttons held, the wheel
		virtual void gamepad(u16 buttons, i16 leftX, i16 leftY, i16 rightX, i16 rightY, u16 leftTrigger, u16 rightTrigger) = 0;
	};

	class HostInput
	{
	public:
		virtual ~HostInput() = default;

		// Hand pending host input - keys, text, the mouse, the gamepad - to `input`. Returns false when the host
		// wants the machine to stop (for example, the window was closed).
		virtual bool pump(InputSink& input) = 0;

		// A file was dropped on the host's window: plug it in. The machine installs the handler before it runs and
		// removes it (an empty function) when it is done, and the host calls it from pump(), on the machine's own
		// thread, so a host without a window to drop on never calls it.
		virtual void setFileDropHandler(std::function<void(const std::filesystem::path&)>) {}
	};
}

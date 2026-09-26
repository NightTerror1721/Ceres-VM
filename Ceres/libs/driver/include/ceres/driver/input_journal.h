#pragma once

#include <ceres/core/base/types.h>
#include <ceres/driver/host_backend.h>
#include <ceres/devices/terminal/terminal.h>
#include <ceres/vm/interrupt_controller.h>
#include <array>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Everything the host gives the machine, in one place (plan/v2 SPEC 3.3). Input arrives on the host's own threads
// and times - stdin, the console, the window - and reaching a device then would make a run depend on the host's
// timing. So it waits here, and the runner injects it between two slices of the machine's time, stamped with the
// cycle it went in at. The same stamps are what --record writes and --replay feeds back, cycle for cycle.
namespace ceres::driver
{
	struct InputEvent
	{
		enum class Kind : u8
		{
			TerminalBytes,   // bytes for the terminal's input (`data`)
			TerminalClose,   // the terminal's input ended
			Key,             // a key: `values[0]` the code, `values[1]` 1 pressed or 0 released
			Text,            // typed text, UTF-8 (`data`)
			Mouse,           // relative motion: dx, dy, buttons, wheel
			Gamepad,         // buttons, left x, left y, right x, right y, left trigger, right trigger
			FileDrop,        // a file dropped on the window (`data`, its path)
			Quit,            // the host closed the machine (its window): a replay stops here
			Reset,           // the machine restarted, and its cycles with it: what follows counts from 0 again
		};

		u64 cycle = 0;
		Kind kind = Kind::TerminalBytes;
		std::string data;
		std::array<i32, 7> values{};

		friend bool operator==(const InputEvent&, const InputEvent&) = default;
	};

	// One line of a recording: "<cycle> <kind> <arguments>", strings in hex so any byte survives.
	std::string formatInputEvent(const InputEvent& event);
	std::optional<InputEvent> parseInputEvent(std::string_view line);

	// A whole recording, as --record writes it: a header line, then one event a line.
	std::expected<std::vector<InputEvent>, std::string> readInputRecording(std::istream& in);

	// What the host's input reaches between two slices.
	struct InputTargets
	{
		devices::TerminalDevice& terminal;
		devices::KeyboardDevice& keyboard;
		devices::MouseDevice& mouse;
		devices::GamepadDevice& gamepad;
		std::function<void(const std::filesystem::path&)> drop;   // plugs a dropped file in; may be empty
	};

	class InputHub final : public InputSink
	{
	public:
		static inline constexpr std::string_view RecordingHeader = "ceres-input 1";

	private:
		std::mutex _mutex;
		std::deque<InputEvent> _queue;                // posted by the host, not yet injected (cycle unset)
		vm::InterruptController* _wake = nullptr;     // poked when something is posted, so a halted wait ends
		std::ostream* _record = nullptr;
		std::vector<InputEvent> _replay;
		usize _nextReplay = 0;
		bool _replaying = false;

		void apply(const InputEvent& event, InputTargets& targets);
		void write(const InputEvent& event);

	public:
		// `wake` is the machine's interrupt controller, poked (never raised) when input is posted.
		explicit InputHub(vm::InterruptController* wake = nullptr) : _wake(wake) {}

		// --record: every event injected is written here as it goes in.
		void record(std::ostream& out);
		// --replay: the events are injected at their cycles, and whatever the host posts is ignored.
		void replay(std::vector<InputEvent> events);
		bool replaying() const noexcept { return _replaying; }

		// From any thread. Ignored while replaying. Bytes for the terminal join the bytes already waiting.
		void post(InputEvent event);

		// The machine is gone: a reader thread that outlives it may still post, but nothing is poked any more.
		void disconnect();

		// InputSink: what a window's pump() hands over.
		void key(u32 code, bool pressed) override;
		void text(std::string_view utf8) override;
		void mouse(i32 dx, i32 dy, u8 buttons, i8 wheel) override;
		void gamepad(u16 buttons, i16 leftX, i16 leftY, i16 rightX, i16 rightY, u16 leftTrigger, u16 rightTrigger) override;

		// The injection point, on the machine's thread between two slices: everything waiting goes in at `cycle`.
		// Terminal bytes go in only as far as the terminal's ring has room; the rest wait for the next point.
		// False when a replay reaches the point where the recording's host quit.
		bool inject(u64 cycle, InputTargets& targets);

		// The host quit at `cycle` (its window was closed): recorded, so a replay stops there too.
		void quit(u64 cycle);

		// The machine restarted (the system control device's reset): its cycles start from 0 again.
		void restarted();
	};
}

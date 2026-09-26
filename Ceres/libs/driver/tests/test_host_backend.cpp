#include "framework.h"
#include <ceres/driver/driver.h>
#include <ceres/driver/host_backend.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string_view>
#include <vector>

using namespace ceres;
using namespace ceres::driver;
using namespace ceres::testing;

namespace
{
	// A host with a keyboard: on its second visit it types "h" and Enter. The second, not the first, so a
	// program has had a slice of instructions to ask for raw keys before anything is typed.
	class TypingBackend final : public HostBackend
	{
	public:
		int pumps = 0;

		bool pump(InputSink& input) override
		{
			if (++pumps == 2)
			{
				input.text("h");
				input.key(devices::scancode::Return, true);
			}
			return true;
		}
		void present(const devices::DisplayDevice&) override {}
	};

	std::string runTyped(const char* name, const char* program)
	{
		const auto source = std::filesystem::temp_directory_path() / name;
		{
			std::ofstream file{source};
			file << program;
		}
		const HostBackendFactory factory = []() -> std::unique_ptr<HostBackend> { return std::make_unique<TypingBackend>(); };
		std::istringstream input;
		std::ostringstream output;
		std::ostringstream diagnostics;
		const int result = execute(RunCommand{.input = source, .window = true}, {&input, &output, &diagnostics}, factory);
		std::filesystem::remove(source);
		return (result == 0 ? std::string{} : "exit " + std::to_string(result) + ": ") + output.str() + diagnostics.str();
	}

	// What a host that shows text in a window was asked to do. It lives in the test, because the backend is gone
	// when the run is over.
	struct WindowStats
	{
		int windowsCreated = 0;
		int windowsOpened = 0;
		std::vector<std::string> frames;   // the first row of each frame it was given
	};

	class TextWindowBackend final : public HostBackend
	{
	public:
		TextWindowBackend(WindowStats& stats, bool canDraw, bool canOpen) : _stats(stats), _canDraw(canDraw), _canOpen(canOpen) {}

		bool pump(InputSink&) override { return true; }
		void present(const devices::DisplayDevice&) override {}
		bool showsText() const noexcept override { return true; }
		bool openWindow() override { ++_stats.windowsOpened; return _canOpen; }
		bool presentText(const devices::FramebufferDevice::Frame& frame) override
		{
			if (!_canDraw)
				return false;
			_stats.frames.emplace_back(frame.cells.begin(), frame.cells.begin() + frame.width);
			return true;
		}

	private:
		WindowStats& _stats;
		bool _canDraw;
		bool _canOpen;
	};

	// The program: put a letter in the top left cell and present, once per letter, with a wait after each long
	// enough for the host to have had its turn (it looks between slices of 4096 instructions).
	std::string presentProgram(std::string_view letters)
	{
		std::string text = "@text\nglobal main:\n    la   r13, 0xFF030000\n";
		int label = 0;
		for (char letter : letters)
		{
			text += "    li   r0, " + std::to_string(static_cast<int>(letter)) + "\n";
			text += "    str  [r13 + 0x0C], r0\n";      // one cell, at the cursor
			text += "    li   r0, 2\n";
			text += "    str  [r13 + 0], r0\n";         // present
			text += "    li   r5, 3000\n.wait" + std::to_string(label) + ":\n    sub  r5, r5, 1\n    cmp  r5, 0\n    jnz  .wait" + std::to_string(label) + "\n";
			++label;
		}
		text += "    la   r7, 0xFFFF0000\n    li   r0, 1\n    str  [r7 + 0], r0\n";
		return text;
	}

	struct Run { int result; std::string output; std::string diagnostics; WindowStats stats; };

	Run runProgram(const std::string& program, RunCommand command, bool haveFactory = true, bool canDraw = true, bool canOpen = true)
	{
		const auto source = std::filesystem::temp_directory_path() / "ceres_text_window_test.casm";
		{
			std::ofstream file{source};
			file << program;
		}
		command.input = source;
		WindowStats stats;
		const HostBackendFactory factory = [&stats, canDraw, canOpen]() -> std::unique_ptr<HostBackend>
		{
			++stats.windowsCreated;
			return std::make_unique<TextWindowBackend>(stats, canDraw, canOpen);
		};
		std::istringstream input;
		std::ostringstream output;
		std::ostringstream diagnostics;
		const int result = execute(command, {&input, &output, &diagnostics}, haveFactory ? factory : HostBackendFactory{});
		std::filesystem::remove(source);
		return { result, output.str(), diagnostics.str(), stats };
	}

	void setHeadless(const char* value)
	{
#if defined(_WIN32)
		_putenv_s("CERES_HEADLESS", value);
#else
		if (*value) setenv("CERES_HEADLESS", value, 1); else unsetenv("CERES_HEADLESS");
#endif
	}

	std::string quietProgram()
	{
		return "@text\nglobal main:\n    la   r7, 0xFFFF0000\n    li   r0, 1\n    str  [r7 + 0], r0\n";
	}

	// A backend that records how often the loop calls it, without any window. It is what proves the
	// cooperative loop (pump -> slice -> present) runs, without needing SDL in the test.
	class RecordingBackend final : public HostBackend
	{
	public:
		int pumps = 0;
		int presents = 0;

		bool pump(InputSink&) override
		{
			++pumps;
			return true;
		}
		void present(const devices::DisplayDevice&) override { ++presents; }
	};
}

TEST(driver_command, run_accepts_the_window_flag)
{
	char program[] = "ceres";
	char run[] = "run";
	char input[] = "demo.casm";
	char window[] = "--window";
	char* argv[] = { program, run, input, window };
	auto parsed = parseCommandLine(4, argv);
	CHECK(parsed.has_value());
	const auto* command = std::get_if<RunCommand>(&*parsed);
	CHECK(command != nullptr);
	CHECK(command->window);
}

TEST(driver_window, window_without_a_backend_is_a_clean_error)
{
	std::istringstream input;
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int result = execute(RunCommand{.input = "no-such.casm", .window = true},
		{&input, &output, &diagnostics}, {});

	CHECK_EQ(result, 1);
	CHECK(diagnostics.str().find("windowed host") != std::string::npos);
}

TEST(driver_window, a_windowed_run_drives_the_cooperative_loop)
{
	const auto source = std::filesystem::temp_directory_path() / "ceres_driver_window_test.casm";
	{
		std::ofstream file{source};
		file << "@text\n"
			"global main:\n"
			"    la r13, 0xFF000004\n"
			"    li r0, 65\n"
			"    str  [r13 + 0], r0\n"
			"    la r13, 0xFFFF0000\n"
			"    li r0, 1\n"
			"    str  [r13 + 0], r0\n";
	}

	RecordingBackend* captured = nullptr;
	const HostBackendFactory factory = [&captured]() -> std::unique_ptr<HostBackend>
	{
		auto backend = std::make_unique<RecordingBackend>();
		captured = backend.get();
		return backend;
	};

	std::istringstream input;
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int result = execute(RunCommand{.input = source, .window = true},
		{&input, &output, &diagnostics}, factory);
	std::filesystem::remove(source);

	CHECK_EQ(result, 0);
	CHECK(captured != nullptr);
	if (!captured) return;

	// The program printed its byte and shut down; the loop ran at least one pump/present cycle.
	CHECK_EQ(output.str(), std::string{ "A" });
	CHECK(captured->pumps >= 1);
	CHECK(captured->presents >= 1);
}

TEST(driver_window, what_is_typed_in_the_window_reaches_a_program_reading_the_terminal)
{
	// No raw request: the window's keystrokes are also terminal bytes, so a program reading its input as a
	// stream sees "h" and the Enter as a newline.
	const std::string output = runTyped("ceres_window_typed_bytes.casm",
		"@text\n"
		"global main:\n"
		"    la   r13, 0xFF000000\n"
		"    li   r4, 0\n"
		".wait:\n"
		"    ldr  r1, [r13 + 0]\n"
		"    and  r2, r1, 1\n"
		"    cmp  r2, 0\n"
		"    jz   .wait\n"
		"    ldr  r3, [r13 + 8]\n"
		"    str  [r13 + 4], r3\n"
		"    add  r4, r4, 1\n"
		"    cmp  r4, 2\n"
		"    jnz  .wait\n"
		"    la   r7, 0xFFFF0000\n"
		"    li   r0, 1\n"
		"    str  [r7 + 0], r0\n");
	CHECK_EQ(output, std::string{ "h\n" });
}

TEST(driver_window, a_program_that_asked_for_raw_keys_reads_them_and_the_terminal_stays_empty)
{
	// It writes the terminal's mode register (0x18) with the raw bit, reads the keyboard's key register
	// (0x0C) until a keystroke is there, prints it, and then says whether the terminal got a byte too.
	// It must not: the keys were given once, on the keyboard.
	const std::string output = runTyped("ceres_window_raw_keys.casm",
		"@text\n"
		"global main:\n"
		"    la   r13, 0xFF000000\n"
		"    la   r12, 0xFF050000\n"
		"    li   r0, 1\n"
		"    str  [r13 + 0x18], r0\n"
		"    ldr  r5, [r13 + 0x18]\n"       // what the host granted: raw (1) and keystrokes (2)
		"    add  r5, r5, 48\n"
		"    str  [r13 + 4], r5\n"
		".wait:\n"
		"    ldr  r1, [r12 + 0]\n"
		"    and  r2, r1, 4\n"
		"    cmp  r2, 0\n"
		"    jz   .wait\n"
		"    ldr  r3, [r12 + 0x0C]\n"
		"    str  [r13 + 4], r3\n"
		"    ldr  r1, [r13 + 0]\n"
		"    and  r2, r1, 1\n"
		"    cmp  r2, 0\n"
		"    jz   .none\n"
		"    li   r3, 84\n"                  // 'T': the terminal got a byte as well
		"    jp   .say\n"
		".none:\n"
		"    li   r3, 78\n"                  // 'N'
		".say:\n"
		"    str  [r13 + 4], r3\n"
		"    la   r7, 0xFFFF0000\n"
		"    li   r0, 1\n"
		"    str  [r7 + 0], r0\n");
	CHECK_EQ(output, std::string{ "3hN" });
}

// --- The text framebuffer in the window, and out of it -----------------------------------------------

TEST(driver_text_window, a_machine_with_a_screen_shows_a_presented_frame_in_the_window_and_not_on_the_terminal)
{
	setHeadless("");
	// No --window: the host is asked for anyway, and would open its window when the frame arrives.
	const Run run = runProgram(presentProgram("X"), RunCommand{});
	CHECK_EQ(run.result, 0);
	CHECK_EQ(run.stats.windowsCreated, 1);
	CHECK_EQ(run.stats.windowsOpened, 0);            // the window is the host's to open, when the first frame comes
	CHECK(run.output.empty());                       // nothing on stdout
	CHECK_EQ(run.stats.frames.size(), usize{ 1 });
	CHECK(run.stats.frames[0].starts_with("X"));
}

TEST(driver_text_window, each_frame_the_program_presents_in_its_own_slice_reaches_the_window)
{
	setHeadless("");
	const Run run = runProgram(presentProgram("XYZ"), RunCommand{});
	CHECK_EQ(run.result, 0);
	CHECK_EQ(run.stats.frames.size(), usize{ 3 });
	if (run.stats.frames.size() == 3)
	{
		CHECK(run.stats.frames[0].starts_with("X"));
		CHECK(run.stats.frames[1].starts_with("Y"));
		CHECK(run.stats.frames[2].starts_with("Z"));
	}
}

TEST(driver_text_window, a_program_that_shows_nothing_is_never_given_a_frame_to_draw)
{
	setHeadless("");
	const Run run = runProgram(quietProgram(), RunCommand{});
	CHECK_EQ(run.result, 0);
	CHECK_EQ(run.stats.windowsOpened, 0);
	CHECK(run.stats.frames.empty());
	CHECK(run.output.empty());
}

TEST(driver_text_window, terminal_keeps_the_window_out_of_it_and_the_frame_goes_to_stdout)
{
	setHeadless("");
	const Run run = runProgram(presentProgram("X"), RunCommand{.terminal = true});
	CHECK_EQ(run.result, 0);
	CHECK_EQ(run.stats.windowsCreated, 0);           // the host was not even asked for
	CHECK(run.output.starts_with("X"));
	CHECK_EQ(std::count(run.output.begin(), run.output.end(), '\n'), 20);   // the whole 40 x 20 grid
}

TEST(driver_text_window, the_environment_can_say_the_same_as_terminal)
{
	setHeadless("1");
	const Run run = runProgram(presentProgram("X"), RunCommand{});
	setHeadless("");
	CHECK_EQ(run.result, 0);
	CHECK_EQ(run.stats.windowsCreated, 0);
	CHECK(run.output.starts_with("X"));
}

TEST(driver_text_window, zero_or_false_in_the_environment_does_not_mean_headless)
{
	for (const char* value : { "0", "false" })
	{
		setHeadless(value);
		const Run run = runProgram(presentProgram("X"), RunCommand{});
		CHECK_EQ(run.stats.windowsCreated, 1);
	}
	setHeadless("");
}

TEST(driver_text_window, window_opens_it_at_once_and_wins_over_the_environment)
{
	setHeadless("1");
	const Run run = runProgram(quietProgram(), RunCommand{.window = true});
	setHeadless("");
	CHECK_EQ(run.result, 0);
	CHECK_EQ(run.stats.windowsCreated, 1);
	CHECK_EQ(run.stats.windowsOpened, 1);            // at once: the program showed nothing at all
}

TEST(driver_text_window, a_window_that_cannot_be_opened_when_asked_for_by_name_is_an_error)
{
	setHeadless("");
	const Run run = runProgram(quietProgram(), RunCommand{.window = true}, true, true, false);
	CHECK_EQ(run.result, 1);
	CHECK(run.diagnostics.find("open a window") != std::string::npos);
}

TEST(driver_text_window, a_build_without_a_windowed_host_prints_the_frame_as_it_always_did)
{
	setHeadless("");
	const Run run = runProgram(presentProgram("X"), RunCommand{}, false);
	CHECK_EQ(run.result, 0);
	CHECK(run.output.starts_with("X"));
}

TEST(driver_text_window, a_host_without_text_support_leaves_the_frame_to_the_terminal)
{
	setHeadless("");
	// RecordingBackend, a pixel-only host, does not say it shows text.
	const auto source = std::filesystem::temp_directory_path() / "ceres_text_window_pixels.casm";
	{
		std::ofstream file{source};
		file << presentProgram("X");
	}
	const HostBackendFactory factory = []() -> std::unique_ptr<HostBackend> { return std::make_unique<RecordingBackend>(); };
	std::istringstream input;
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int result = execute(RunCommand{.input = source}, {&input, &output, &diagnostics}, factory);
	std::filesystem::remove(source);
	CHECK_EQ(result, 0);
	CHECK(output.str().starts_with("X"));
}

TEST(driver_text_window, a_host_that_cannot_draw_gives_the_frame_and_every_later_one_to_the_terminal)
{
	setHeadless("");
	const Run run = runProgram(presentProgram("XY"), RunCommand{}, true, false);
	CHECK_EQ(run.result, 0);
	CHECK(run.stats.frames.empty());
	// The first frame could not be drawn, so it went to the terminal; the machine then stopped offering the
	// window, so the second went there without being tried.
	const usize frames = static_cast<usize>(std::count(run.output.begin(), run.output.end(), '\n')) / 20;
	CHECK_EQ(frames, usize{ 2 });
}

TEST(driver_text_window, terminal_and_window_together_are_refused_and_neither_belongs_to_the_other_commands)
{
	char program[] = "ceres";
	char run[] = "run";
	char input[] = "demo.casm";
	char window[] = "--window";
	char terminal[] = "--terminal";
	char* both[] = { program, run, input, window, terminal };
	CHECK(!parseCommandLine(5, both).has_value());

	char* only[] = { program, run, input, terminal };
	auto parsed = parseCommandLine(4, only);
	CHECK(parsed.has_value());
	const auto* command = std::get_if<RunCommand>(&*parsed);
	CHECK(command != nullptr && command->terminal && !command->window);

	char assemble[] = "asm";
	char* wrong[] = { program, assemble, input, terminal };
	CHECK(!parseCommandLine(4, wrong).has_value());
}

TEST(driver_window, a_program_halted_for_a_key_gets_it_from_the_next_pump)
{
	// The program sleeps in HALT until the keyboard's interrupt. The window only hands keys over in
	// pump(), between slices of steps, and a halted step used to sleep a millisecond: a slice of 4096
	// of them held the next pump back for four seconds. A halted machine now ends its slice.
	const char* program =
		"interrupt 19: on_key\n"
		"@text\n"
		"global main:\n"
		"    sti\n"
		".wait:\n"
		"    halt\n"
		"    jp .wait\n"
		"on_key:\n"
		"    li r0, 107\n"
		"    la r13, 0xFF000004\n"
		"    str  [r13 + 0], r0\n"
		"    li r0, 1\n"
		"    la r13, 0xFFFF0000\n"
		"    str  [r13 + 0], r0\n"
		"    iret\n";

	const auto start = std::chrono::steady_clock::now();
	const std::string shown = runTyped("ceres_window_halt.casm", program);
	const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

	CHECK_EQ(shown, std::string{ "k" });
	CHECK(took < 2000);
}

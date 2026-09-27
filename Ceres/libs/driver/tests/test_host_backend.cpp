#include "framework.h"
#include <ceres/driver/driver.h>
#include <ceres/devices/input/keyboard.h>

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
	// A test host that is both the window's input and its screen, as the SDL window is.
	template <class Host>
	WindowHost windowOf(std::shared_ptr<Host> host)
	{
		return WindowHost{ host, host, nullptr };
	}

	// A host with a keyboard: on its second visit it types "h" and Enter. The second, not the first, so a
	// program has had a slice of instructions to ask for raw keys before anything is typed.
	class TypingBackend final : public HostInput, public VideoOutput
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
		bool openWindow(u32, u32) override { return true; }
		void present(const devices::video::VideoFrame&) override {}
	};

	std::string runTyped(const char* name, const char* program)
	{
		const auto source = std::filesystem::temp_directory_path() / name;
		{
			std::ofstream file{source};
			file << program;
		}
		const WindowHostFactory factory = [] { return windowOf(std::make_shared<TypingBackend>()); };
		std::istringstream input;
		std::ostringstream output;
		std::ostringstream diagnostics;
		const int result = execute(RunCommand{.input = source, .window = true}, {&input, &output, &diagnostics}, factory);
		std::filesystem::remove(source);
		return (result == 0 ? std::string{} : "exit " + std::to_string(result) + ": ") + output.str() + diagnostics.str();
	}

	// What a windowed host was asked to do. It lives in the test, because the host is gone when the run is over.
	struct WindowStats
	{
		int windowsCreated = 0;
		int windowsOpened = 0;
		u32 openedWidth = 0, openedHeight = 0;
		int frames = 0;
		u32 firstPixel = 0;                   // of the last frame
		std::vector<HostStatus> statuses;
		bool fullscreen = false;
	};

	// A window that opens (or not, when told so), counts the frames it is given and the statuses, and says it is open,
	// so the machine keeps it after the program ends - until the key it presses on the first pump after that.
	class FrameWindow final : public HostInput, public VideoOutput
	{
	private:
		WindowStats& _stats;
		bool _canOpen;
		bool _open = false;

	public:
		FrameWindow(WindowStats& stats, bool canOpen) : _stats(stats), _canOpen(canOpen) {}

		bool pump(InputSink& input) override
		{
			if (!_stats.statuses.empty() && _stats.statuses.back().exitCode)
				input.key(devices::scancode::Space, true);
			return true;
		}
		bool openWindow(u32 width, u32 height) override
		{
			++_stats.windowsOpened;
			_stats.openedWidth = width;
			_stats.openedHeight = height;
			_open = _canOpen;
			return _canOpen;
		}
		void present(const devices::video::VideoFrame& frame) override
		{
			++_stats.frames;
			_stats.firstPixel = frame.pixels.empty() ? 0 : frame.pixels[0];
		}
		bool windowOpen() const noexcept override { return _open; }
		void setFullscreen(bool fullscreen) override { _stats.fullscreen = fullscreen; }
		void setStatus(const HostStatus& status) override { _stats.statuses.push_back(status); }
	};

	struct Run { int result; std::string output; std::string diagnostics; WindowStats stats; };

	Run runProgram(const std::string& program, RunCommand command, bool haveFactory = true, bool canOpen = true)
	{
		const auto source = std::filesystem::temp_directory_path() / "ceres_window_test.casm";
		{
			std::ofstream file{source};
			file << program;
		}
		command.input = source;
		WindowStats stats;
		const WindowHostFactory factory = [&stats, canOpen]
		{
			++stats.windowsCreated;
			return windowOf(std::make_shared<FrameWindow>(stats, canOpen));
		};
		std::istringstream input;
		std::ostringstream output;
		std::ostringstream diagnostics;
		const int result = execute(command, {&input, &output, &diagnostics}, haveFactory ? factory : WindowHostFactory{});
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

	// Shuts the machine down with `code`.
	std::string shutdown(int code = 0)
	{
		return "    la   r7, 0xFFFF0000\n    la   r0, " + std::to_string(1 | (code << 8)) + "\n    str  [r7 + 0], r0\n";
	}

	// Waits until the GPU has counted `frames` vertical blanks, after `before` (code that runs first).
	std::string frameProgram(int frames, const std::string& before = {}, int code = 0)
	{
		return "@text\nglobal main:\n    la   r13, 0xFF400000\n" + before +
			".wait:\n    ldr  r1, [r13 + 0x118]\n    cmp  r1, " + std::to_string(frames) + "\n    jnz  .wait\n" + shutdown(code);
	}

	// A background colour, a Present, and two more frames so the Present is applied and shown.
	std::string presentProgram()
	{
		return frameProgram(2, "    la   r0, 0x123456\n    str  [r13 + 0x11C], r0\n    li   r0, 1\n    str  [r13 + 0x120], r0\n");
	}

	// A text framebuffer frame with an X in its corner, presented: it goes to the terminal.
	std::string textFrameProgram()
	{
		return "@text\nglobal main:\n    la   r13, 0xFF440000\n    li   r0, 88\n    str  [r13 + 0x0C], r0\n"
			"    li   r0, 2\n    str  [r13 + 0], r0\n" + shutdown();
	}

	// A backend that records how often the loop calls it. It is what proves the cooperative loop (pump -> slice ->
	// present) runs, without needing SDL in the test.
	class RecordingBackend final : public HostInput, public VideoOutput
	{
	public:
		int pumps = 0;
		int presents = 0;

		bool pump(InputSink&) override
		{
			++pumps;
			return true;
		}
		bool openWindow(u32, u32) override { return true; }
		void present(const devices::video::VideoFrame&) override { ++presents; }
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

TEST(driver_command, run_takes_the_window_options_and_the_other_commands_do_not)
{
	char program[] = "ceres";
	char run[] = "run";
	char input[] = "demo.casm";
	char fullscreen[] = "--fullscreen";
	char exitOnHalt[] = "--exit-on-halt";
	char frames[] = "--frames";
	char dir[] = "shots";
	char refresh[] = "--refresh";
	char fifty[] = "50";
	char* argv[] = { program, run, input, fullscreen, exitOnHalt, frames, dir, refresh, fifty };
	auto parsed = parseCommandLine(9, argv);
	CHECK(parsed.has_value());
	const auto* command = parsed ? std::get_if<RunCommand>(&*parsed) : nullptr;
	CHECK(command != nullptr && command->fullscreen && command->exitOnHalt && command->framesDir == "shots" && command->refresh == 50u);

	char seventy[] = "70";
	char* bad[] = { program, run, input, refresh, seventy };
	CHECK(!parseCommandLine(5, bad).has_value());
	char disasm[] = "disasm";
	char* wrong[] = { program, disasm, input, fullscreen };
	CHECK(!parseCommandLine(4, wrong).has_value());
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
		file << frameProgram(3, "    la   r12, 0xFF000004\n    li   r0, 65\n    str  [r12 + 0], r0\n");
	}

	RecordingBackend* captured = nullptr;
	const WindowHostFactory factory = [&captured]
	{
		auto backend = std::make_shared<RecordingBackend>();
		captured = backend.get();
		return windowOf(backend);
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

	// The program printed its byte, waited three frames and shut down; the loop pumped and presented.
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
		"    la   r12, 0xFF100000\n"
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

// --- The screen in the window (plan/v2 F5.5) ------------------------------------------------------------

TEST(driver_screen, the_window_opens_at_the_start_at_the_gpus_resolution_and_shows_its_frames)
{
	setHeadless("");
	const Run run = runProgram(presentProgram(), RunCommand{});
	CHECK_EQ(run.result, 0);
	CHECK_EQ(run.stats.windowsCreated, 1);
	CHECK_EQ(run.stats.windowsOpened, 1);
	CHECK_EQ(run.stats.openedWidth, 640u);
	CHECK_EQ(run.stats.openedHeight, 480u);
	CHECK(run.stats.frames >= 1);
	CHECK_EQ(run.stats.firstPixel, 0x123456u);   // the last frame shown has the background the program set
}

TEST(driver_screen, a_window_shows_one_frame_a_vertical_blank_in_real_time)
{
	setHeadless("");
	// 30 frames at 60 Hz in real time: half a second, and every one of them reaches the window.
	RunCommand command;
	command.speed = Speed::realtime();
	const auto start = std::chrono::steady_clock::now();
	const Run run = runProgram(frameProgram(30), command);
	const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	CHECK_EQ(run.result, 0);
	CHECK(run.stats.frames >= 29 && run.stats.frames <= 30);
	CHECK(took >= 450);
}

TEST(driver_screen, the_window_stays_after_the_program_and_says_how_it_ended)
{
	setHeadless("");
	const Run run = runProgram(frameProgram(1, {}, 3), RunCommand{});
	CHECK_EQ(run.result, 3);
	CHECK(!run.stats.statuses.empty());
	if (!run.stats.statuses.empty())
	{
		CHECK_EQ(run.stats.statuses.back().profile, std::string("standard"));
		CHECK(run.stats.statuses.back().exitCode == 3);
	}
}

TEST(driver_screen, exit_on_halt_closes_it_at_once)
{
	setHeadless("");
	RunCommand command;
	command.exitOnHalt = true;
	command.fullscreen = true;
	const Run run = runProgram(frameProgram(1), command);
	CHECK_EQ(run.result, 0);
	CHECK(run.stats.fullscreen);
	CHECK(std::none_of(run.stats.statuses.begin(), run.stats.statuses.end(), [](const HostStatus& s) { return s.exitCode.has_value(); }));
}

TEST(driver_screen, frames_writes_a_png_for_every_present)
{
	setHeadless("1");
	const auto dir = std::filesystem::temp_directory_path() / "ceres_frames_test";
	std::filesystem::remove_all(dir);
	RunCommand command;
	command.framesDir = dir;
	// Two Presents, each on a frame of its own.
	const std::string present = "    li   r0, 1\n    str  [r13 + 0x120], r0\n";
	const Run run = runProgram("@text\nglobal main:\n    la   r13, 0xFF400000\n" + present +
		".a:\n    ldr  r1, [r13 + 0x118]\n    cmp  r1, 1\n    jnz  .a\n" + present +
		".b:\n    ldr  r1, [r13 + 0x118]\n    cmp  r1, 3\n    jnz  .b\n" + shutdown(), command);
	setHeadless("");
	CHECK_EQ(run.result, 0);
	CHECK(std::filesystem::exists(dir / "frame_000000.png"));
	CHECK(std::filesystem::exists(dir / "frame_000001.png"));
	CHECK(!std::filesystem::exists(dir / "frame_000002.png"));
	std::ifstream png(dir / "frame_000000.png", std::ios::binary);
	std::string head(24, '\0');
	png.read(head.data(), 24);
	CHECK(head.starts_with("\x89PNG\r\n\x1A\n"));
	// IHDR's width and height, big-endian: 640 x 480.
	CHECK_EQ(static_cast<u8>(head[18]), u8{ 0x02 });
	CHECK_EQ(static_cast<u8>(head[19]), u8{ 0x80 });
	CHECK_EQ(static_cast<u8>(head[23]), u8{ 0xE0 });
	png.close();
	std::filesystem::remove_all(dir);
}

TEST(driver_screen, terminal_keeps_the_window_out_of_it)
{
	setHeadless("");
	const Run run = runProgram(frameProgram(1), RunCommand{.terminal = true});
	CHECK_EQ(run.result, 0);
	CHECK_EQ(run.stats.windowsCreated, 0);           // the host was not even asked for
}

TEST(driver_screen, the_environment_can_say_the_same_as_terminal)
{
	setHeadless("1");
	const Run run = runProgram(frameProgram(1), RunCommand{});
	setHeadless("");
	CHECK_EQ(run.result, 0);
	CHECK_EQ(run.stats.windowsCreated, 0);
}

TEST(driver_screen, zero_or_false_in_the_environment_does_not_mean_headless)
{
	for (const char* value : { "0", "false" })
	{
		setHeadless(value);
		const Run run = runProgram(frameProgram(1), RunCommand{});
		CHECK_EQ(run.stats.windowsCreated, 1);
	}
	setHeadless("");
}

TEST(driver_screen, window_wins_over_the_environment)
{
	setHeadless("1");
	const Run run = runProgram(frameProgram(1), RunCommand{.window = true});
	setHeadless("");
	CHECK_EQ(run.result, 0);
	CHECK_EQ(run.stats.windowsCreated, 1);
	CHECK_EQ(run.stats.windowsOpened, 1);
}

TEST(driver_screen, a_window_that_cannot_be_opened_when_asked_for_by_name_is_an_error)
{
	setHeadless("");
	const Run run = runProgram(frameProgram(1), RunCommand{.window = true}, true, false);
	CHECK_EQ(run.result, 1);
	CHECK(run.diagnostics.find("open a window") != std::string::npos);
}

TEST(driver_screen, without_a_display_the_machine_runs_without_a_window)
{
	setHeadless("");
	const Run run = runProgram(frameProgram(1), RunCommand{}, true, false);
	CHECK_EQ(run.result, 0);
	CHECK_EQ(run.stats.frames, 0);
}

TEST(driver_screen, the_v1_text_framebuffer_goes_to_the_terminal_until_it_is_retired)
{
	setHeadless("");
	const Run run = runProgram(textFrameProgram(), RunCommand{});
	CHECK_EQ(run.result, 0);
	CHECK(run.output.starts_with("X"));
}

TEST(driver_command, terminal_and_window_together_are_refused_and_neither_belongs_to_the_other_commands)
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
		"interrupt 20: on_key\n"
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

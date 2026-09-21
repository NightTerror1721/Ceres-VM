#include "framework.h"
#include <ceres/driver/driver.h>
#include <ceres/driver/host_backend.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

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

		bool pump(devices::KeyboardDevice& keyboard, devices::MouseDevice&, devices::GamepadDevice&) override
		{
			if (++pumps == 2)
			{
				keyboard.pushText(std::string_view{ "h" });
				keyboard.pushKey(devices::scancode::Return, true);
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

	// A backend that records how often the loop calls it, without any window. It is what proves the
	// cooperative loop (pump -> slice -> present) runs, without needing SDL in the test.
	class RecordingBackend final : public HostBackend
	{
	public:
		int pumps = 0;
		int presents = 0;

		bool pump(devices::KeyboardDevice&, devices::MouseDevice&, devices::GamepadDevice&) override
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
			"    strb [r13 + 0], r0\n"
			"    la r13, 0xFFFF0000\n"
			"    li r0, 1\n"
			"    strb [r13 + 0], r0\n";
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
		"    ldrb r1, [r13 + 0]\n"
		"    and  r2, r1, 1\n"
		"    cmp  r2, 0\n"
		"    jz   .wait\n"
		"    ldrb r3, [r13 + 8]\n"
		"    strb [r13 + 4], r3\n"
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
		"    strb [r13 + 4], r5\n"
		".wait:\n"
		"    ldr  r1, [r12 + 0]\n"
		"    and  r2, r1, 4\n"
		"    cmp  r2, 0\n"
		"    jz   .wait\n"
		"    ldr  r3, [r12 + 0x0C]\n"
		"    strb [r13 + 4], r3\n"
		"    ldrb r1, [r13 + 0]\n"
		"    and  r2, r1, 1\n"
		"    cmp  r2, 0\n"
		"    jz   .none\n"
		"    li   r3, 84\n"                  // 'T': the terminal got a byte as well
		"    jp   .say\n"
		".none:\n"
		"    li   r3, 78\n"                  // 'N'
		".say:\n"
		"    strb [r13 + 4], r3\n"
		"    la   r7, 0xFFFF0000\n"
		"    li   r0, 1\n"
		"    str  [r7 + 0], r0\n");
	CHECK_EQ(output, std::string{ "3hN" });
}

// Loading and running a program from inside the machine (plan/v2 F7.1): the system control device's command 3 starts
// a .cres of the host directory in place of the running program, and `ceres run --shell` - or `ceres run` without a
// program - goes back to the shell whenever a program ends, with its exit status in CERES_STATUS.

#include "framework.h"
#include "run_capture.h"
#include <ceres/asm/assembler.h>
#include <ceres/driver/command.h>
#include <ceres/driver/driver.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace ceres;
using namespace ceres::driver;
using namespace ceres::testing;

namespace
{
	// The first time (an empty environment) it prints 'A' and runs child.cres with argv { "child", "hi" } and the
	// environment { "WHO=parent" }; if that store comes back - the load failed - it prints 'F' and ends with status
	// 9. Started again (the shell coming back), it prints its environment, each entry followed by ';', and ends
	// with status 0.
	const char* Parent = R"(
@rodata
    let CHILD: u8[] = "child.cres\0"
    let ARG0: u8[] = "child\0"
    let ARG1: u8[] = "hi\0"
    let ENV0: u8[] = "WHO=parent\0"
@bss
    let argv: u32[2]
    let envp: u32[2]
    let block: u32[3]
@text
global main:
    la   r4, 0xFF000004
    ldr  r3, [r2 + 0]
    ifne r3, 0, .back
    li   r5, 65
    str  [r4 + 0], r5
    la   r6, argv
    la   r7, ARG0
    str  [r6 + 0], r7
    la   r7, ARG1
    str  [r6 + 4], r7
    la   r8, envp
    la   r7, ENV0
    str  [r8 + 0], r7
    li   r7, 0
    str  [r8 + 4], r7
    la   r9, block
    li   r7, 2
    str  [r9 + 0], r7
    str  [r9 + 4], r6
    str  [r9 + 8], r8
    la   r13, 0xFFFF0000
    la   r7, CHILD
    str  [r13 + 0x30], r7
    str  [r13 + 0x34], r9
    li   r7, 3
    str  [r13 + 0], r7
    li   r5, 70
    str  [r4 + 0], r5
    la   r0, 0x0901
    str  [r13 + 0], r0
    halt
.back:
    mov  r10, r2
.next:
    ldr  r3, [r10 + 0]
    ifeq r3, 0, .done
.letter:
    ldrb r5, [r3 + 0]
    ifeq r5, 0, .separator
    str  [r4 + 0], r5
    add  r3, r3, 1
    jmp  .letter
.separator:
    li   r5, 59
    str  [r4 + 0], r5
    add  r10, r10, 4
    jmp  .next
.done:
    la   r13, 0xFFFF0000
    li   r0, 1
    str  [r13 + 0], r0
    halt
)";

	// Prints 'B' and its argv[1], and ends with status 7.
	const char* Child = R"(
@text
global main:
    la   r4, 0xFF000004
    li   r5, 66
    str  [r4 + 0], r5
    ldr  r3, [r1 + 4]
.letter:
    ldrb r5, [r3 + 0]
    ifeq r5, 0, .end
    str  [r4 + 0], r5
    add  r3, r3, 1
    jmp  .letter
.end:
    la   r13, 0xFFFF0000
    la   r0, 0x0701
    str  [r13 + 0], r0
    halt
)";

	// A fresh directory with `source` assembled into it as `name`.
	void assembleInto(const std::filesystem::path& directory, const char* name, const char* source)
	{
		std::filesystem::create_directories(directory);
		const auto sourcePath = directory / "source.casm";
		{
			std::ofstream file(sourcePath, std::ios::binary | std::ios::trunc);
			file << source;
		}
		casm::Assembler assembler;
		auto program = assembler.assemble({ sourcePath });
		CHECK(program.has_value() && !assembler.hasErrors());
		if (program)
			CHECK(program->saveToFile(directory / name).has_value());
		std::filesystem::remove(sourcePath);
	}

	// A host directory with child.cres in it, and a sysroot whose shell is the parent.
	struct Setup
	{
		std::filesystem::path root = uniqueTempPath("ceres_load");
		std::filesystem::path host = root / "host";
		std::filesystem::path sysroot = root / "sysroot";

		explicit Setup(bool withChild = true)
		{
			std::filesystem::create_directories(host);
			if (withChild)
				assembleInto(host, "child.cres", Child);
			assembleInto(sysroot / "bin", "shell.cres", Parent);
		}
		~Setup() { std::filesystem::remove_all(root); }
	};
}

TEST(driver_load, a_program_runs_another_and_the_shell_comes_back_with_its_exit_status)
{
	const Setup setup;
	const auto screenLog = uniqueTempPath("ceres_load_screen");
	const CapturedRun result = captureRun(RunCommand{ .input = {}, .hostDirectory = setup.host, .screenLog = screenLog, .sysroot = setup.sysroot });
	const std::string screen = readWhole(screenLog);
	std::filesystem::remove(screenLog);
	CHECK_EQ(result.diagnostics, std::string{});
	CHECK_EQ(result.status, 0);
	// The child had the parent's arguments and environment; the shell came back with that environment and the status.
	CHECK_EQ(result.output, std::string{ "ABhiWHO=parent;CERES_STATUS=7;" });
	// Neither start cleared the screen: all three wrote on the same line.
	CHECK(screen.find("--- end ---\nABhiWHO=parent;CERES_STATUS=7;") != std::string::npos);
}

TEST(driver_load, without_the_shell_the_run_ends_with_the_program_it_loaded)
{
	const Setup setup;
	const CapturedRun result = captureRun(RunCommand{ .input = setup.sysroot / "bin" / "shell.cres", .hostDirectory = setup.host });
	CHECK_EQ(result.status, 7);
	CHECK_EQ(result.output, std::string{ "ABhi" });
}

TEST(driver_load, shell_runs_the_program_given_first_and_then_the_shell)
{
	const Setup setup;
	const CapturedRun result = captureRun(RunCommand{ .input = setup.host / "child.cres", .arguments = { "yo" },
		.hostDirectory = setup.host, .shell = true, .sysroot = setup.sysroot });
	CHECK_EQ(result.status, 0);
	CHECK_EQ(result.output, std::string{ "ByoCERES_STATUS=7;" });
}

TEST(driver_load, a_load_that_fails_returns_to_the_program_and_the_log_says_why)
{
	const Setup setup{ false };
	const CapturedRun result = captureRun(RunCommand{ .input = setup.sysroot / "bin" / "shell.cres", .hostDirectory = setup.host });
	CHECK_EQ(result.status, 9);
	CHECK_EQ(result.output, std::string{ "AF" });
	CHECK(result.diagnostics.find("Cannot load 'child.cres'") != std::string::npos);
	// The program's name for it, not where the host keeps it.
	CHECK(result.diagnostics.find(setup.root.filename().string()) == std::string::npos);

	// Without a host directory there is nothing to load from.
	const CapturedRun bare = captureRun(RunCommand{ .input = setup.sysroot / "bin" / "shell.cres" });
	CHECK_EQ(bare.output, std::string{ "AF" });
	CHECK(bare.diagnostics.find("no host directory") != std::string::npos);
}

TEST(driver_load, without_a_shell_to_start_the_run_says_where_it_looked)
{
	const Setup setup;
	const auto empty = setup.root / "empty";
	std::filesystem::create_directories(empty);
	const CapturedRun result = captureRun(RunCommand{ .input = {}, .sysroot = empty });
	CHECK_EQ(result.status, 1);
	CHECK(result.diagnostics.find("there is no shell at") != std::string::npos);
	CHECK(result.diagnostics.find("shell.cres") != std::string::npos);
}

TEST(driver_command, run_without_a_program_is_the_shell_and_shell_and_sysroot_belong_to_run)
{
	char program[] = "ceres";
	char run[] = "run";
	char shell[] = "--shell";
	char sysroot[] = "--sysroot";
	char dir[] = "C:/ceres";
	char input[] = "demo.cres";
	char assemble[] = "asm";

	char* alone[] = { program, run };
	auto parsed = parseCommandLine(2, alone);
	CHECK(parsed.has_value() && std::get<RunCommand>(*parsed).input.empty());

	char* both[] = { program, run, input, shell, sysroot, dir };
	parsed = parseCommandLine(6, both);
	CHECK(parsed.has_value());
	if (parsed)
	{
		const RunCommand& command = std::get<RunCommand>(*parsed);
		CHECK(command.shell);
		CHECK_EQ(command.sysroot.string(), std::string("C:/ceres"));
		CHECK_EQ(command.input.string(), std::string("demo.cres"));
	}

	// --shell goes back to the shell after a program: without one it means nothing.
	char* shellAlone[] = { program, run, shell };
	CHECK(!parseCommandLine(3, shellAlone).has_value());
	char* notRun[] = { program, assemble, input, shell };
	CHECK(!parseCommandLine(4, notRun).has_value());
	// A command other than run still needs its input.
	char* noInput[] = { program, assemble };
	CHECK(!parseCommandLine(2, noInput).has_value());
}

#include "framework.h"
#include "run_capture.h"
#include <ceres/driver/command.h>
#include <ceres/driver/driver.h>
#include <ceres/driver/machine.h>
#include <ceres/asm/assembler.h>
#include <ceres/asm/object_file.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <functional>
#include <optional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace ceres::driver;
using namespace ceres::testing;

TEST(driver_command, bare_path_is_run)
{
	char program[] = "ceres";
	char input[] = "demo.casm";
	char* argv[] = { program, input };
	auto parsed = parseCommandLine(2, argv);
	CHECK(parsed.has_value());
	CHECK(std::holds_alternative<RunCommand>(*parsed));
}

TEST(driver_command, invalid_memory_is_a_usage_error)
{
	char program[] = "ceres";
	char run[] = "run";
	char input[] = "demo.casm";
	char memory[] = "--ram";
	char value[] = "nope";
	char* argv[] = { program, run, input, memory, value };
	auto parsed = parseCommandLine(5, argv);
	CHECK(!parsed.has_value());

	// A size the machine cannot have is refused here, not by an exception out of the machine (plan/v2 SPEC 2).
	for (const char* size : { "100000", "4096", "2147487744", "3G", "0", "12Q" })
	{
		std::string text = size;
		argv[4] = text.data();
		CHECK(!parseCommandLine(5, argv).has_value());
	}
	for (const char* size : { "2147483648", "2G", "64K", "8192", "1024M" })
	{
		std::string text = size;
		argv[4] = text.data();
		CHECK(parseCommandLine(5, argv).has_value());
	}

	// --memory is gone: --ram took its place (plan/v2 SPEC 10).
	char old[] = "--memory";
	char size[] = "65536";
	char* oldArgv[] = { program, run, input, old, size };
	CHECK(!parseCommandLine(5, oldArgv).has_value());
}

TEST(driver_command, archive_has_an_output_not_a_fake_input)
{
	char program[] = "ceres";
	char ar[] = "ar";
	char output[] = "library.car";
	char object[] = "member.cobj";
	char* argv[] = { program, ar, output, object };
	auto parsed = parseCommandLine(4, argv);
	CHECK(parsed.has_value());
	const auto* archive = std::get_if<ArchiveCommand>(&*parsed);
	CHECK(archive != nullptr);
	CHECK_EQ(archive->output.string(), std::string("library.car"));
	CHECK_EQ(archive->inputs.size(), std::size_t{1});
}

TEST(driver_command, options_from_another_command_are_rejected)
{
	char program[] = "ceres";
	char run[] = "run";
	char input[] = "demo.casm";
	char json[] = "--json";
	char* argv[] = { program, run, input, json };
	auto parsed = parseCommandLine(4, argv);
	CHECK(!parsed.has_value());
}

TEST(driver_command, symtab_belongs_to_link_alone)
{
	char program[] = "ceres";
	char link[] = "link";
	char run[] = "run";
	char input[] = "main.cobj";
	char dashO[] = "-o";
	char output[] = "main.cres";
	char symtab[] = "--symtab";
	char* linkArgv[] = { program, link, input, dashO, output, symtab };
	auto linked = parseCommandLine(6, linkArgv);
	CHECK(linked.has_value());
	const auto* command = linked ? std::get_if<LinkCommand>(&*linked) : nullptr;
	CHECK(command != nullptr && command->symbolTable);

	char* plainArgv[] = { program, link, input, dashO, output };
	auto plain = parseCommandLine(5, plainArgv);
	const auto* without = plain ? std::get_if<LinkCommand>(&*plain) : nullptr;
	CHECK(without != nullptr && !without->symbolTable);

	char* runArgv[] = { program, run, input, symtab };
	CHECK(!parseCommandLine(4, runArgv).has_value());
}

TEST(driver_command, gc_sections_belongs_to_link_alone)
{
	char program[] = "ceres";
	char link[] = "link";
	char run[] = "run";
	char input[] = "main.cobj";
	char dashO[] = "-o";
	char output[] = "main.cres";
	char gc[] = "--gc-sections";
	char* linkArgv[] = { program, link, input, dashO, output, gc };
	auto linked = parseCommandLine(6, linkArgv);
	CHECK(linked.has_value());
	const auto* command = linked ? std::get_if<LinkCommand>(&*linked) : nullptr;
	CHECK(command != nullptr && command->gcSections);

	char* plainArgv[] = { program, link, input, dashO, output };
	auto plain = parseCommandLine(5, plainArgv);
	const auto* without = plain ? std::get_if<LinkCommand>(&*plain) : nullptr;
	CHECK(without != nullptr && !without->gcSections);

	char* runArgv[] = { program, run, input, gc };
	CHECK(!parseCommandLine(4, runArgv).has_value());
}

TEST(driver_command, strict_mmio_belongs_to_run_alone)
{
	char program[] = "ceres";
	char run[] = "run";
	char profile[] = "profile";
	char input[] = "main.casm";
	char strict[] = "--strict-mmio";
	char* runArgv[] = { program, run, input, strict };
	auto parsed = parseCommandLine(4, runArgv);
	const auto* command = parsed ? std::get_if<RunCommand>(&*parsed) : nullptr;
	CHECK(command != nullptr && command->strictMmio);

	char* profileArgv[] = { program, profile, input, strict };
	CHECK(!parseCommandLine(4, profileArgv).has_value());
}

TEST(driver_run, strict_mmio_turns_an_undeclared_device_register_into_a_fault)
{
	// 0x40 in the terminal's slot is no register of its. Without --strict-mmio it reads 0 and the program
	// shuts down with status 0; with it, the load is a MemoryFault, whose handler shuts down with status 7.
	const auto source = std::filesystem::temp_directory_path() / "ceres_driver_strict_test.casm";
	{
		std::ofstream file{source};
		file << "interrupt MemoryFault: on_fault\n"
			"@text\n"
			"global main:\n"
			"    la   r13, 0xFF000040\n"
			"    ldr  r1, [r13 + 0]\n"
			"    la   r13, 0xFFFF0000\n"
			"    li   r0, 1\n"
			"    str  [r13 + 0], r0\n"
			"on_fault:\n"
			"    la   r13, 0xFFFF0000\n"
			"    la   r0, 0x0701\n"            // shut down, status 7
			"    str  [r13 + 0], r0\n";
	}
	std::istringstream input;
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int lenient = execute(RunCommand{.input = source}, {&input, &output, &diagnostics});
	RunCommand strictRun{.input = source};
	strictRun.strictMmio = true;
	const int strict = execute(strictRun, {&input, &output, &diagnostics});
	std::filesystem::remove(source);

	CHECK_EQ(lenient, 0);
	CHECK_EQ(strict, 7);
}

TEST(driver_command, rtc_takes_a_utc_moment_and_belongs_to_run_alone)
{
	char program[] = "ceres";
	char run[] = "run";
	char profile[] = "profile";
	char input[] = "main.casm";
	char option[] = "--rtc";
	char moment[] = "2026-09-26T12:00:00";
	char* runArgv[] = { program, run, input, option, moment };
	auto parsed = parseCommandLine(5, runArgv);
	const auto* command = parsed ? std::get_if<RunCommand>(&*parsed) : nullptr;
	CHECK(command != nullptr && command->rtc == ceres::i64{ 1'790'424'000 });

	char* profileArgv[] = { program, profile, input, option, moment };
	CHECK(!parseCommandLine(5, profileArgv).has_value());

	for (const char* wrong : { "2026-02-30T00:00:00", "1969-12-31T23:59:59", "2026-09-26 12:00:00", "2026-09-26T24:00:00", "2026-09-26T-1:00:00", "2026-09-26T12:-5:00", "tomorrow" })
	{
		std::string text = wrong;
		char* wrongArgv[] = { program, run, input, option, text.data() };
		CHECK(!parseCommandLine(5, wrongArgv).has_value());
	}
}

TEST(driver_run, rtc_sets_what_the_program_reads_from_the_real_time_clock)
{
	// The program exits with the low byte of the timer's Rtc register as its status.
	const auto source = std::filesystem::temp_directory_path() / "ceres_driver_rtc_test.casm";
	{
		std::ofstream file{source};
		file << "@text\n"
			"global main:\n"
			"    la   r13, 0xFF01001C\n"
			"    ldr  r1, [r13 + 0]\n"
			"    shl  r1, r1, 8\n"
			"    or   r1, r1, 1\n"
			"    la   r13, 0xFFFF0000\n"
			"    str  [r13 + 0], r1\n";
	}
	std::istringstream input;
	std::ostringstream output;
	std::ostringstream diagnostics;
	RunCommand command{.input = source};
	command.rtc = 42;                           // 1970-01-01T00:00:42
	const int status = execute(command, {&input, &output, &diagnostics});
	std::filesystem::remove(source);
	CHECK_EQ(status, 42);
}

TEST(driver_command, speed_and_cpu_clock_belong_to_run_alone)
{
	char program[] = "ceres";
	char run[] = "run";
	char profile[] = "profile";
	char input[] = "main.casm";
	char speed[] = "--speed";
	char twice[] = "2x";
	char clock[] = "--cpu-clock";
	char mhz[] = "8MHz";
	char* runArgv[] = { program, run, input, speed, twice, clock, mhz };
	auto parsed = parseCommandLine(7, runArgv);
	const auto* command = parsed ? std::get_if<RunCommand>(&*parsed) : nullptr;
	CHECK(command != nullptr && command->speed == Speed::parse("2x"));
	CHECK(command != nullptr && command->machine.cpuClockHz == ceres::u64{ 8000000 });
	CHECK(command != nullptr && command->machine.id == ProfileId::Custom);   // a loose option makes it custom

	char* profileArgv[] = { program, profile, input, speed, twice };
	CHECK(!parseCommandLine(5, profileArgv).has_value());

	// The CPU goes up to 400 MHz (plan/v2 SPEC 4).
	for (const char* good : { "50000000", "50M", "50m", "25kHz", "400M", "400000000" })
	{
		std::string text = good;
		char* argv[] = { program, run, input, clock, text.data() };
		CHECK(parseCommandLine(5, argv).has_value());
	}
	for (const char* wrong : { "0", "fast", "1G", "400000001", "M", "-1" })
	{
		std::string text = wrong;
		char* argv[] = { program, run, input, clock, text.data() };
		CHECK(!parseCommandLine(5, argv).has_value());
	}
	std::string sluggish = "slow";
	char* badSpeed[] = { program, run, input, speed, sluggish.data() };
	CHECK(!parseCommandLine(5, badSpeed).has_value());
}

namespace
{
	// Arms the countdown for `cycles`, halts until it has run out (masked: a request just ends the halt, and any
	// request does - the terminal's, when the input closes - so it halts again while Countdown reads what is
	// left), and exits with status 3. How long the host takes over it is up to --speed.
	int runWait(ceres::u32 cycles, std::optional<Speed> speed, std::optional<ceres::u64> clockHz = {})
	{
		const auto source = std::filesystem::temp_directory_path() / "ceres_driver_speed_test.casm";
		{
			std::ofstream file{source};
			file << "@text\n"
				"global main:\n"
				"    la   r13, 0xFF010000\n"
				"    la   r1, " << cycles << "\n"
				"    str  [r13 + 8], r1\n"
				".wait:\n"
				"    halt\n"
				"    ldr  r2, [r13 + 8]\n"
				"    cmp  r2, 0\n"
				"    jnz  .wait\n"
				"    la   r13, 0xFFFF0000\n"
				"    la   r0, 0x0301\n"
				"    str  [r13 + 0], r0\n";
		}
		std::istringstream input;
		std::ostringstream output;
		std::ostringstream diagnostics;
		RunCommand command{.input = source};
		command.speed = speed;
		if (clockHz)
			command.machine.cpuClockHz = *clockHz;
		const int status = execute(command, {&input, &output, &diagnostics});
		std::filesystem::remove(source);
		return status;
	}

	long long millisecondsOf(const std::function<void()>& run)
	{
		const auto start = std::chrono::steady_clock::now();
		run();
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	}
}

TEST(driver_run, realtime_takes_the_machines_time_in_host_time)
{
	int status = 0;
	const long long took = millisecondsOf([&] { status = runWait(10'000'000, Speed::realtime()); });   // 200 ms at 50 MHz
	CHECK_EQ(status, 3);
	CHECK(took >= 180);
	CHECK(took < 2000);
}

TEST(driver_run, max_takes_whatever_the_host_takes)
{
	int status = 0;
	const long long took = millisecondsOf([&] { status = runWait(10'000'000, Speed::unlimited()); });
	CHECK_EQ(status, 3);
	CHECK(took < 150);                          // the halt jumps the 200 ms, and nothing waits for them
}

TEST(driver_run, a_factor_and_the_cpu_clock_scale_the_wait)
{
	// 1 000 000 cycles at 10 MHz are 100 ms of machine time; at 0.5x they take 200 ms of the host's.
	int status = 0;
	const long long took = millisecondsOf([&] { status = runWait(1'000'000, Speed{ false, 0.5 }, 10'000'000); });
	CHECK_EQ(status, 3);
	CHECK(took >= 180);
	CHECK(took < 2000);
}

TEST(driver_command, run_takes_the_program_arguments_after_a_double_dash_and_env)
{
	char program[] = "ceres";
	char run[] = "run";
	char input[] = "prog.cres";
	char env[] = "--env";
	char pair[] = "HOME=/save";
	char dashes[] = "--";
	char flag[] = "-x";
	char word[] = "y";
	char* argv[] = { program, run, input, env, pair, dashes, flag, word };
	auto parsed = parseCommandLine(8, argv);
	CHECK(parsed.has_value());
	const auto* command = parsed ? std::get_if<RunCommand>(&*parsed) : nullptr;
	CHECK(command != nullptr);
	if (!command) return;
	CHECK_EQ(command->arguments.size(), ceres::usize{ 2 });   // an option after -- is the program's
	CHECK(command->arguments.size() == 2 && command->arguments[0] == "-x" && command->arguments[1] == "y");
	CHECK(command->environment.size() == 1 && command->environment[0] == "HOME=/save");

	char asmCommand[] = "asm";
	char* asmArgv[] = { program, asmCommand, input, dashes, word };
	CHECK(!parseCommandLine(5, asmArgv).has_value());   // only run starts a program
	char bare[] = "HOME";
	char* badArgv[] = { program, run, input, env, bare };
	CHECK(!parseCommandLine(5, badArgv).has_value());   // NAME=value

	char hostDir[] = "--host-dir";
	char dir[] = "saves";
	char* hostArgv[] = { program, run, input, hostDir, dir };
	auto host = parseCommandLine(5, hostArgv);
	const auto* hosted = host ? std::get_if<RunCommand>(&*host) : nullptr;
	CHECK(hosted != nullptr && hosted->hostDirectory == std::filesystem::path("saves"));
	char* asmHostArgv[] = { program, asmCommand, input, hostDir, dir };
	auto refused = parseCommandLine(5, asmHostArgv);
	CHECK(!refused.has_value());
	CHECK(!refused && refused.error().message.find("--host-dir") != std::string::npos);   // names what was there
	char* lonelyArgv[] = { program, asmCommand, input, dashes };
	auto lonely = parseCommandLine(4, lonelyArgv);
	CHECK(!lonely && lonely.error().message.find("'--'") != std::string::npos);
}

TEST(driver_machine, main_receives_argc_argv_and_envp_and_the_registers_say_the_same)
{
	const auto source = std::filesystem::temp_directory_path() / "ceres_driver_arguments_test.casm";
	{
		std::ofstream file{source};
		file << "@text\n"
			"global main:\n"
			"    mov r10, r1\n"                 // argv
			"    mov r11, r2\n"                 // envp
			"    la r13, 0xFF000004\n"
			"    add r3, r0, 48\n"              // argc as a digit
			"    str  [r13 + 0], r3\n"
			"    ldr r4, [r10 + 4]\n"           // argv[1][0]
			"    ldrb r5, [r4 + 0]\n"
			"    str  [r13 + 0], r5\n"
			"    ldr r4, [r11 + 0]\n"           // envp[0][0]
			"    ldrb r5, [r4 + 0]\n"
			"    str  [r13 + 0], r5\n"
			"    la r12, 0xFFFF0000\n"
			"    ldr r6, [r12 + 24]\n"          // ArgumentCountRegister
			"    add r6, r6, 48\n"
			"    str  [r13 + 0], r6\n"
			"    ldr r7, [r12 + 28]\n"          // ArgumentVectorRegister: argv[2][0]
			"    ldr r8, [r7 + 8]\n"
			"    ldrb r9, [r8 + 0]\n"
			"    str  [r13 + 0], r9\n"
			"    ldr r7, [r7 + 12]\n"           // argv[argc] is a null pointer
			"    cmp r7, 0\n"
			"    jz .done\n"
			"    li r9, 33\n"
			"    str  [r13 + 0], r9\n"
			".done:\n"
			"    li r0, 1\n"
			"    str  [r12 + 0], r0\n";
	}
	ceres::casm::Assembler assembler;
	auto program = assembler.assemble({source});
	std::filesystem::remove(source);
	CHECK(program.has_value());
	if (!program) return;

	std::string output;
	MachineConfig config;
	config.arguments = { "prog", "x", "y" };
	config.environment = { "K=v" };
	Machine machine{config, {
		.terminalOutput = [&output](std::span<const ceres::u8> bytes)
		{
			output.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		}
	}};
	CHECK(machine.load(*program).has_value());
	CHECK(machine.run().has_value());
	CHECK_EQ(output, std::string{"3xK3y"});
}

TEST(driver_command, json_diagnostics_stay_on_the_output_stream)
{
	const auto source = std::filesystem::temp_directory_path() / "ceres_driver_invalid_test.casm";
	{
		std::ofstream file{source};
		file << "this is not Ceres assembly\n";
	}

	std::istringstream input;
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int result = execute(AssembleCommand{.inputs = {source}, .jsonDiagnostics = true},
		{&input, &output, &diagnostics});
	std::filesystem::remove(source);

	CHECK_EQ(result, 1);
	CHECK(output.str().starts_with("["));
	CHECK(diagnostics.str().empty());
}

TEST(driver_command, missing_input_is_an_operational_error)
{
	std::istringstream input;
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int result = execute(RunCommand{.input = "definitely-missing.casm"},
		{&input, &output, &diagnostics});

	CHECK_EQ(result, 1);
	CHECK(diagnostics.str().starts_with("No such file:"));
}

TEST(driver_machine, host_receives_terminal_output)
{
	const auto source = std::filesystem::temp_directory_path() / "ceres_driver_machine_test.casm";
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
	ceres::casm::Assembler assembler;
	auto program = assembler.assemble({source});
	std::filesystem::remove(source);
	CHECK(program.has_value());
	if (!program) return;

	std::string output;
	Machine machine{{}, {
		.terminalOutput = [&output](std::span<const ceres::u8> bytes)
		{
			output.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		}
	}};
	CHECK(machine.load(*program).has_value());
	CHECK(machine.run().has_value());
	CHECK_EQ(output, std::string{"A"});
	CHECK_EQ(machine.droppedInputBytes(), ceres::u64{0});
}

TEST(driver_run, typed_input_reaches_a_program_that_reads_it_late)
{
	// The program is busy for a while before it reads anything; the 300 characters typed with --type wait for it
	// in the terminal (a line without its Enter goes in whole when the input ends). Each byte is echoed as it
	// is read.
	const auto source = std::filesystem::temp_directory_path() / "ceres_driver_flow_test.casm";
	{
		std::ofstream file{source};
		file << "@text\n"
			"global main:\n"
			"    la   r6, 0xFF000000\n"
			"    la   r7, 0xFFFF0000\n"
			"    la   r2, 300000\n"
			".spin:\n"
			"    sub  r2, r2, 1\n"
			"    ifne r2, 0, .spin\n"
			"    la   r3, 300\n"
			".next:\n"
			"    la   r4, 3000000\n"          // patience per byte, so a lost byte ends the run
			".poll:\n"
			"    ldr  r5, [r6 + 12]\n"            // bytes available: the status word always has bit 1 (ready for output) set
			"    ifne r5, 0, .got\n"
			"    sub  r4, r4, 1\n"
			"    ifne r4, 0, .poll\n"
			"    jp   .done\n"
			".got:\n"
			"    ldr  r5, [r6 + 8]\n"
			"    str  [r6 + 4], r5\n"
			"    sub  r3, r3, 1\n"
			"    ifne r3, 0, .next\n"
			".done:\n"
			"    li   r0, 1\n"
			"    str  [r7 + 0], r0\n";
	}

	std::string sent;
	for (int i = 0; i < 300; ++i)
		sent.push_back(static_cast<char>('!' + (i * 7) % 90));

	const CapturedRun run = captureRun(RunCommand{.input = source}, sent);
	std::filesystem::remove(source);

	CHECK_EQ(run.status, 0);
	CHECK_EQ(run.output.size(), sent.size());
	CHECK_EQ(run.output, sent);
	CHECK(run.hostOutput.empty());   // nothing of the program's reaches the host's stdout
}

TEST(driver_run, unread_input_does_not_keep_the_run_from_ending)
{
	// The reader parks when the ring is full. A program that never reads must still end, and the
	// reader must notice that it did rather than wait for room that will never come.
	const auto source = std::filesystem::temp_directory_path() / "ceres_driver_unread_test.casm";
	{
		std::ofstream file{source};
		file << "@text\n"
			"global main:\n"
			"    la   r7, 0xFFFF0000\n"
			"    li   r0, 1\n"
			"    str  [r7 + 0], r0\n";
	}

	const CapturedRun run = captureRun(RunCommand{.input = source}, std::string(5000, 'x'));
	std::filesystem::remove(source);

	CHECK_EQ(run.status, 0);
	CHECK(run.output.empty());
}

TEST(driver_run, the_exit_status_a_program_writes_becomes_the_run_status)
{
	const auto source = std::filesystem::temp_directory_path() / "ceres_driver_exit_status_test.casm";
	{
		std::ofstream file{source};
		file << "@text\n"
			"global main:\n"
			"    la   r7, 0xFFFF0000\n"
			"    li   r0, 0x0701\n"
			"    str  [r7 + 0], r0\n";
	}

	std::istringstream input;
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int result = execute(RunCommand{.input = source}, {&input, &output, &diagnostics});
	std::filesystem::remove(source);

	CHECK_EQ(result, 7);
}

TEST(driver_run, a_program_can_read_until_the_end_of_its_input)
{
	// Echo stdin until the terminal says the input is over, then leave with status 3. Before the
	// end-of-input bit a program could only spin on "no data yet", which is also what a slow pipe
	// looks like.
	const auto source = std::filesystem::temp_directory_path() / "ceres_driver_eof_test.casm";
	{
		std::ofstream file{source};
		file << "@text\n"
			"global main:\n"
			"    la   r13, 0xFF000000\n"
			".wait:\n"
			"    ldr  r1, [r13 + 0]\n"
			"    and  r2, r1, 1\n"
			"    cmp  r2, 0\n"
			"    jz   .check_end\n"
			"    ldr  r3, [r13 + 8]\n"
			"    str  [r13 + 4], r3\n"
			"    jp   .wait\n"
			".check_end:\n"
			"    and  r2, r1, 4\n"
			"    cmp  r2, 0\n"
			"    jz   .wait\n"
			"    la   r7, 0xFFFF0000\n"
			"    li   r0, 0x0301\n"
			"    str  [r7 + 0], r0\n";
	}

	const std::string sent = std::string(300, 'q') + "\nlast\n";
	const CapturedRun run = captureRun(RunCommand{.input = source}, sent);
	std::filesystem::remove(source);

	CHECK_EQ(run.status, 3);
	CHECK_EQ(run.output, sent);
}

namespace
{
	struct BoundedRun
	{
		std::optional<int> status;   // nothing when the run did not end in time
		std::string output;
		std::string diagnostics;
	};

	// `source` under `ceres run`, on a thread of its own that is given `limit` to end: a run that hangs fails the
	// test that asked instead of holding up the whole suite. One that does not end is left behind, with what it
	// writes to, since nothing can stop it from outside.
	BoundedRun runWithin(const char* name, const std::string& source, std::chrono::seconds limit, std::filesystem::path logFile = {})
	{
		struct Run
		{
			std::filesystem::path path;
			std::filesystem::path transcript;
			std::istringstream input;
			std::ostringstream output;
			std::ostringstream diagnostics;
			std::promise<int> status;
		};
		auto run = std::make_shared<Run>();
		run->path = std::filesystem::temp_directory_path() / name;
		run->transcript = uniqueTempPath("ceres_bounded_transcript");
		{
			std::ofstream file{run->path, std::ios::binary | std::ios::trunc};
			file << source;
		}
		std::future<int> status = run->status.get_future();
		std::thread([run, logFile]
		{
			RunCommand command{.input = run->path, .logFile = logFile};
			command.transcript = run->transcript;
			run->status.set_value(execute(command, {&run->input, &run->output, &run->diagnostics}));
		}).detach();
		if (status.wait_for(limit) != std::future_status::ready)
			return {};
		std::filesystem::remove(run->path);
		std::string transcript = readWhole(run->transcript);
		std::filesystem::remove(run->transcript);
		return { status.get(), std::move(transcript), run->diagnostics.str() };
	}
}

TEST(driver_run, an_unhandled_fault_ends_the_run_with_status_1_and_says_what_it_was)
{
	// A word load from an odd address, with no AlignmentFault handler bound. The BIOS's default handler used
	// to print 'E' and halt, and nothing ever woke the machine: the run hung.
	const BoundedRun run = runWithin("ceres_driver_unhandled_fault_test.casm",
		"@text\n"
		"global main:\n"
		"    li   r1, 0x601\n"
		"    ldr  r2, [r1 + 0]\n"
		"    la   r13, 0xFFFF0000\n"
		"    li   r0, 1\n"
		"    str  [r13 + 0], r0\n",
		std::chrono::seconds(10));

	CHECK(run.status.has_value());
	if (!run.status) return;
	CHECK_EQ(*run.status, 1);
	CHECK(run.output.empty());
	// main starts at 0x400 and the load is its second instruction.
	CHECK_EQ(run.diagnostics, std::string("[ceres:error] Unhandled AlignmentFault at 0x00000404: read of 4 bytes at 0x00000601 (FaultReason 1, Alignment)\n"));
}

TEST(driver_run, an_unhandled_fault_on_a_device_register_reports_its_fault_reason)
{
	// A byte store to the terminal's Output register: devices take aligned 32-bit accesses only (plan/v2 SPEC
	// 5.1), so it is a MemoryFault whose FaultReason is MmioWidth.
	const BoundedRun run = runWithin("ceres_driver_unhandled_mmio_fault_test.casm",
		"@text\n"
		"global main:\n"
		"    la   r13, 0xFF000004\n"
		"    li   r0, 65\n"
		"    strb [r13 + 0], r0\n"
		"    la   r13, 0xFFFF0000\n"
		"    li   r0, 1\n"
		"    str  [r13 + 0], r0\n",
		std::chrono::seconds(10));

	CHECK(run.status.has_value());
	if (!run.status) return;
	CHECK_EQ(*run.status, 1);
	CHECK(run.output.empty());
	CHECK(run.diagnostics.starts_with("[ceres:error] Unhandled MemoryFault at 0x"));
	CHECK(run.diagnostics.find(": write of 1 bytes at 0xFF000004 (FaultReason 5, MmioWidth)\n") != std::string::npos);
}

TEST(driver_run, an_unhandled_trap_ends_the_run_too)
{
	// Not only faults: every vector the BIOS gives a default handler ends the run the same way. A trap goes back
	// to the instruction after it, which is the address it reports.
	const BoundedRun run = runWithin("ceres_driver_unhandled_trap_test.casm",
		"@text\n"
		"global main:\n"
		"    trap\n"
		"    la   r13, 0xFFFF0000\n"
		"    li   r0, 1\n"
		"    str  [r13 + 0], r0\n",
		std::chrono::seconds(10));

	CHECK(run.status.has_value());
	if (!run.status) return;
	CHECK_EQ(*run.status, 1);
	CHECK_EQ(run.diagnostics, std::string("[ceres:error] Unhandled Trap at 0x00000404\n"));
}

namespace
{
	// Two lines of the debug log (plan/v2 SPEC 5.7), a warning and an info one, and then a trap nobody handles.
	constexpr const char* DebugLogProgram =
		"@text\n"
		"global main:\n"
		"    la   r13, 0xFF030000\n"
		"    li   r0, 1\n"
		"    str  [r13 + 4], r0\n"
		"    li   r0, 72\n"
		"    str  [r13 + 0], r0\n"
		"    li   r0, 105\n"
		"    str  [r13 + 0], r0\n"
		"    li   r0, 10\n"
		"    str  [r13 + 0], r0\n"
		"    li   r0, 2\n"
		"    str  [r13 + 4], r0\n"
		"    li   r0, 33\n"
		"    str  [r13 + 0], r0\n"
		"    str  [r13 + 8], r0\n"
		"    ldr  r1, [r13 + 16]\n"
		"    add  r1, r1, 48\n"
		"    la   r13, 0xFF000004\n"
		"    str  [r13 + 0], r1\n"
		"    trap\n";
}

TEST(driver_run, the_debug_log_and_the_diagnostics_go_to_the_hosts_log)
{
	// Without --log: stderr, as "[ceres:<level>] <line>", and the program sees the log collected (Enabled 1).
	const BoundedRun run = runWithin("ceres_driver_debug_log_test.casm", DebugLogProgram, std::chrono::seconds(10));
	CHECK(run.status.has_value());
	if (!run.status) return;
	CHECK_EQ(run.output, std::string("1"));
	CHECK_EQ(run.diagnostics, std::string("[ceres:warn] Hi\n[ceres:info] !\n[ceres:error] Unhandled Trap at 0x00000454\n"));
}

TEST(driver_run, log_sends_the_hosts_log_to_a_file)
{
	const std::filesystem::path logFile = std::filesystem::temp_directory_path() / "ceres_driver_debug_log_test.log";
	std::filesystem::remove(logFile);
	const BoundedRun run = runWithin("ceres_driver_debug_log_file_test.casm", DebugLogProgram, std::chrono::seconds(10), logFile);
	CHECK(run.status.has_value());
	if (!run.status) return;
	CHECK(run.diagnostics.empty());
	std::ifstream file{ logFile, std::ios::binary };
	const std::string written{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
	CHECK_EQ(written, std::string("[ceres:warn] Hi\n[ceres:info] !\n[ceres:error] Unhandled Trap at 0x00000454\n"));
	file.close();
	std::filesystem::remove(logFile);

	char program[] = "ceres";
	char asm_[] = "asm";
	char input[] = "demo.casm";
	char log[] = "--log";
	char value[] = "x.log";
	char* argv[] = { program, asm_, input, log, value };
	CHECK(!parseCommandLine(5, argv).has_value());   // run's alone
}

TEST(driver_command, a_profile_is_a_whole_machine_and_a_loose_option_makes_it_custom)
{
	// plan/v2 SPEC 4: --profile picks a row of the table; any other machine option starts from it and makes it custom.
	char program[] = "ceres";
	char run[] = "run";
	char input[] = "main.casm";
	char profileOption[] = "--profile";

	char* bare[] = { program, run, input };
	auto plain = parseCommandLine(3, bare);
	const auto* standard = plain ? std::get_if<RunCommand>(&*plain) : nullptr;
	CHECK(standard != nullptr && standard->machine == defaultMachineProfile());
	CHECK(standard != nullptr && standard->machine.id == ProfileId::Standard && standard->machine.ramBytes == 64u * 1024 * 1024);

	for (const MachineProfile& expected : machineProfiles())
	{
		std::string name{ profileName(expected.id) };
		char* argv[] = { program, run, input, profileOption, name.data() };
		auto parsed = parseCommandLine(5, argv);
		const auto* command = parsed ? std::get_if<RunCommand>(&*parsed) : nullptr;
		CHECK(command != nullptr && command->machine == expected);
		// No profile but custom goes past 1280x720.
		CHECK(expected.id == ProfileId::Custom || (expected.maxWidth <= 1280 && expected.maxHeight <= 720));
	}

	char micro[] = "micro";
	char resolution[] = "--max-resolution";
	char fullHd[] = "1920x1080";
	char* bigger[] = { program, run, input, profileOption, micro, resolution, fullHd };
	auto custom = parseCommandLine(7, bigger);
	const auto* command = custom ? std::get_if<RunCommand>(&*custom) : nullptr;
	CHECK(command != nullptr && command->machine.id == ProfileId::Custom);
	CHECK(command != nullptr && command->machine.maxWidth == 1920u && command->machine.maxHeight == 1080u);
	CHECK(command != nullptr && command->machine.cpuClockHz == machineProfile(ProfileId::Micro).cpuClockHz);   // the rest is micro's

	char tooBig[] = "2560x1440";
	char* wrong[] = { program, run, input, resolution, tooBig };
	CHECK(!parseCommandLine(5, wrong).has_value());

	char unknown[] = "mainframe";
	char* badName[] = { program, run, input, profileOption, unknown };
	CHECK(!parseCommandLine(5, badName).has_value());

	char video[] = "--max-video";
	char v7[] = "V7";
	char* badVideo[] = { program, run, input, video, v7 };
	CHECK(!parseCommandLine(5, badVideo).has_value());

	char gpu[] = "--gpu-clock";
	char twoGhz[] = "2G";
	char* badGpu[] = { program, run, input, gpu, twoGhz };
	CHECK(!parseCommandLine(5, badGpu).has_value());

	// Machine options belong to the commands that run a machine.
	char asmCommand[] = "asm";
	char* onAsm[] = { program, asmCommand, input, profileOption, micro };
	CHECK(!parseCommandLine(5, onAsm).has_value());
	char debugCommand[] = "debug";
	char* onDebug[] = { program, debugCommand, input, profileOption, micro };
	auto debug = parseCommandLine(5, onDebug);
	const auto* debugging = debug ? std::get_if<DebugCommand>(&*debug) : nullptr;
	CHECK(debugging != nullptr && debugging->machine.id == ProfileId::Micro);
}

TEST(driver_run, the_machine_publishes_its_profile)
{
	// SystemControl reads back what the profile made (plan/v2 SPEC 5.7): MemorySize, CpuClockHz, ProfileId and
	// VramSize, which the program writes to the terminal as four words.
	const auto source = std::filesystem::temp_directory_path() / "ceres_driver_profile_test.casm";
	{
		std::ofstream file{ source };
		file << "@text\n"
			"global main:\n"
			"    la   r13, 0xFFFF0000\n"
			"    la   r12, 0xFF000004\n"
			"    ldr  r1, [r13 + 0x04]\n"
			"    ldr  r2, [r13 + 0x24]\n"
			"    ldr  r3, [r13 + 0x28]\n"
			"    ldr  r4, [r13 + 0x38]\n"
			"    li   r0, 0\n"
			".loop:\n"
			"    str  [r12 + 0], r1\n"
			"    shr  r1, r1, 8\n"
			"    str  [r12 + 0], r2\n"
			"    shr  r2, r2, 8\n"
			"    str  [r12 + 0], r3\n"
			"    shr  r3, r3, 8\n"
			"    str  [r12 + 0], r4\n"
			"    shr  r4, r4, 8\n"
			"    add  r0, r0, 1\n"
			"    cmp  r0, 4\n"
			"    jnz  .loop\n"
			"    li   r0, 1\n"
			"    str  [r13 + 0], r0\n";
	}
	for (const MachineProfile& machine : machineProfiles())
	{
		RunCommand command{ .input = source };
		command.machine = machine;
		const CapturedRun run = captureRun(command);
		CHECK_EQ(run.status, 0);
		const std::string bytes = run.output;
		CHECK_EQ(bytes.size(), ceres::usize{ 16 });
		if (bytes.size() != 16)
			continue;
		ceres::u32 words[4] = {};
		for (ceres::usize i = 0; i < 16; ++i)
			words[i % 4] |= static_cast<ceres::u32>(static_cast<unsigned char>(bytes[i])) << (8 * (i / 4));
		CHECK_EQ(words[0], static_cast<ceres::u32>(machine.ramBytes));
		CHECK_EQ(words[1], static_cast<ceres::u32>(machine.cpuClockHz));
		CHECK_EQ(words[2], static_cast<ceres::u32>(machine.id));
		CHECK_EQ(words[3], static_cast<ceres::u32>(machine.vramBytes));
	}
	std::filesystem::remove(source);
}

TEST(driver_command, stdlib_and_import_directories_belong_where_they_are_used)
{
	char program[] = "ceres";
	char asmCommand[] = "asm";
	char link[] = "link";
	char ar[] = "ar";
	char input[] = "main.casm";
	char object[] = "main.cobj";
	char output[] = "main.cres";
	char dashO[] = "-o";
	char stdlib[] = "--stdlib";
	char dashI[] = "-I";
	char directory[] = "libs";

	// asm imports, so it takes both: --stdlib and its own -I directories, in order.
	char* asmArgv[] = { program, asmCommand, input, stdlib, dashI, directory };
	auto assembled = parseCommandLine(6, asmArgv);
	const auto* assemble = assembled ? std::get_if<AssembleCommand>(&*assembled) : nullptr;
	CHECK(assemble != nullptr && assemble->stdlib);
	CHECK(assemble != nullptr && assemble->importDirectories == std::vector<std::filesystem::path>{ std::filesystem::path("libs") });

	// link takes --stdlib: libceres.car joins the objects.
	char* linkArgv[] = { program, link, object, dashO, output, stdlib };
	auto linked = parseCommandLine(6, linkArgv);
	const auto* linkCommand = linked ? std::get_if<LinkCommand>(&*linked) : nullptr;
	CHECK(linkCommand != nullptr && linkCommand->stdlib);

	// -I is for the command that imports; link does not, and ar neither builds nor links.
	char* linkImport[] = { program, link, object, dashO, output, dashI, directory };
	CHECK(!parseCommandLine(7, linkImport).has_value());
	char* arStdlib[] = { program, ar, output, object, stdlib };
	CHECK(!parseCommandLine(5, arStdlib).has_value());

	// -I names a directory; without one it is a usage error, not a silent no-op.
	char* missingValue[] = { program, asmCommand, input, dashI };
	CHECK(!parseCommandLine(4, missingValue).has_value());
}

TEST(driver_command, the_commands_that_assemble_take_stdlib_and_import_directories)
{
	char program[] = "ceres";
	char run[] = "run";
	char profile[] = "profile";
	char debug[] = "debug";
	char disasm[] = "disasm";
	char input[] = "main.casm";
	char stdlib[] = "--stdlib";
	char dashI[] = "-I";
	char directory[] = "libs";

	char* runArgv[] = { program, run, input, stdlib, dashI, directory };
	auto ran = parseCommandLine(6, runArgv);
	const auto* runCommand = ran ? std::get_if<RunCommand>(&*ran) : nullptr;
	CHECK(runCommand != nullptr && runCommand->stdlib);
	CHECK(runCommand != nullptr && runCommand->importDirectories.size() == 1);

	char* profileArgv[] = { program, profile, input, stdlib, dashI, directory };
	auto profiled = parseCommandLine(6, profileArgv);
	const auto* profileCommand = profiled ? std::get_if<ProfileCommand>(&*profiled) : nullptr;
	CHECK(profileCommand != nullptr && profileCommand->stdlib);
	CHECK(profileCommand != nullptr && profileCommand->importDirectories.size() == 1);

	char* debugArgv[] = { program, debug, input, stdlib, dashI, directory };
	auto debugged = parseCommandLine(6, debugArgv);
	const auto* debugCommand = debugged ? std::get_if<DebugCommand>(&*debugged) : nullptr;
	CHECK(debugCommand != nullptr && debugCommand->stdlib);
	CHECK(debugCommand != nullptr && debugCommand->importDirectories.size() == 1);

	char* disasmArgv[] = { program, disasm, input, stdlib, dashI, directory };
	auto disassembled = parseCommandLine(6, disasmArgv);
	const auto* disassembleCommand = disassembled ? std::get_if<DisassembleCommand>(&*disassembled) : nullptr;
	CHECK(disassembleCommand != nullptr && disassembleCommand->stdlib);
	CHECK(disassembleCommand != nullptr && disassembleCommand->importDirectories.size() == 1);
}

TEST(driver_run, stdlib_links_the_library_installed_with_ceres)
{
	// A stand-in standard library: one object that defines a routine the program calls, shipped the
	// way the real one is (stdlib/lib/libceres.car and libceres.decls.casm) and reached through
	// --stdlib's install directory.
	const auto root = uniqueTempPath("ceres_stdlib");
	const auto lib = root / "stdlib" / "lib";
	std::filesystem::create_directories(lib);

	const auto module = root / "module.casm";
	{
		std::ofstream file(module, std::ios::binary | std::ios::trunc);
		file << "@text\n"
			"global lib_put:\n"
			"    la r13, 0xFF000004\n"
			"    str [r13 + 0], r0\n"
			"    ret\n";
	}
	ceres::casm::Assembler assembler;
	auto object = assembler.assembleObject(module);
	CHECK(object.has_value());
	if (!object) return;
	ceres::casm::ObjectArchive archive;
	archive.members.push_back(ceres::casm::ObjectArchive::Member{ "lib", std::move(*object), true });
	CHECK(archive.write(lib / "libceres.car").has_value());
	{
		std::ofstream file(lib / "libceres.decls.casm", std::ios::binary | std::ios::trunc);
		file << "@text\n"
			"global lib_put:\n";
	}

	const auto source = root / "program.casm";
	{
		std::ofstream file(source, std::ios::binary | std::ios::trunc);
		file << "import \"libceres.decls.casm\"\n"
			"@text\n"
			"global main:\n"
			"    li r0, 65\n"
			"    call lib_put\n"
			"    la r13, 0xFFFF0000\n"
			"    li r0, 1\n"
			"    str [r13 + 0], r0\n"
			"    halt\n";
	}

	RunCommand command{ .input = source };
	command.stdlib = true;
	const CapturedRun run = captureRun(command, {}, {}, { root });
	CHECK_EQ(run.status, 0);
	CHECK_EQ(run.output, std::string{ "A" });

	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);
}

#include "framework.h"
#include <ceres/driver/command.h>
#include <ceres/driver/driver.h>
#include <ceres/driver/machine.h>
#include <ceres/asm/assembler.h>

#include <filesystem>
#include <fstream>
#include <sstream>

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
	char memory[] = "--memory";
	char value[] = "nope";
	char* argv[] = { program, run, input, memory, value };
	auto parsed = parseCommandLine(5, argv);
	CHECK(!parsed.has_value());
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

	for (const char* wrong : { "2026-02-30T00:00:00", "1969-12-31T23:59:59", "2026-09-26 12:00:00", "2026-09-26T24:00:00", "tomorrow" })
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

TEST(driver_run, piped_input_longer_than_the_ring_is_held_back_not_dropped)
{
	// The program is busy for a while before it reads anything. The terminal's ring holds 63 bytes,
	// so an input reader that only pushed would have thrown most of the 300 bytes away by then; it has
	// to wait for the program instead. Each byte is echoed as it arrives.
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

	std::istringstream input{sent};
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int result = execute(RunCommand{.input = source}, {&input, &output, &diagnostics});
	std::filesystem::remove(source);

	CHECK_EQ(result, 0);
	CHECK_EQ(output.str().size(), sent.size());
	CHECK_EQ(output.str(), sent);
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

	std::istringstream input{std::string(5000, 'x')};
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int result = execute(RunCommand{.input = source}, {&input, &output, &diagnostics});
	std::filesystem::remove(source);

	CHECK_EQ(result, 0);
	CHECK(output.str().empty());
}

TEST(driver_machine, host_receives_presented_frames)
{
	const auto source = std::filesystem::temp_directory_path() / "ceres_driver_frame_test.casm";
	{
		std::ofstream file{source};
		file << "@text\n"
			"global main:\n"
			"    la r13, 0xFF030000\n"
			"    li r0, 2\n"
			"    str [r13 + 4], r0\n"
			"    li r0, 1\n"
			"    str [r13 + 8], r0\n"
			"    li r0, 88\n"
			"    str [r13 + 12], r0\n"
			"    li r0, 2\n"
			"    str [r13 + 0], r0\n"
			"    la r13, 0xFFFF0000\n"
			"    li r0, 1\n"
			"    str  [r13 + 0], r0\n";
	}
	ceres::casm::Assembler assembler;
	auto program = assembler.assemble({source});
	std::filesystem::remove(source);
	CHECK(program.has_value());
	if (!program) return;

	std::string frame;
	Machine machine{{}, {.framePresented = [&frame](std::string_view value) { frame = value; }}};
	CHECK(machine.load(*program).has_value());
	CHECK(machine.run().has_value());
	CHECK_EQ(frame, std::string{"X \n"});
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
	std::istringstream input{sent};
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int result = execute(RunCommand{.input = source}, {&input, &output, &diagnostics});
	std::filesystem::remove(source);

	CHECK_EQ(result, 3);
	CHECK_EQ(output.str(), sent);
}

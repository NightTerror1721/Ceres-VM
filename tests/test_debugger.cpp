// Driving the machine one instruction at a time and stopping it on purpose. DebugSession answers
// in plain structs precisely so these can drive it the way a real debugger does, with no protocol
// or editor in the way.

#include "framework.h"
#include "assembler/assembler.h"
#include "debug/debug_session.h"
#include "vm/memory.h"
#include <filesystem>
#include <fstream>

using namespace ceres;
using namespace ceres::testing;

namespace
{
	constexpr u32 TextStart = static_cast<u32>(vm::Memory::UnrestrictedSegmentStartValue);

	// A source file on disk for the length of one test. DebugSession takes paths, not text: it
	// assembles the way the real command line does, and the line table is keyed on the path.
	class TempSource
	{
	private:
		std::filesystem::path _path;

	public:
		TempSource(std::string_view source, std::string_view stem)
		{
			_path = std::filesystem::temp_directory_path() / std::format("ceres_dbg_{}.casm", stem);
			std::ofstream file(_path, std::ios::binary | std::ios::trunc);
			file.write(source.data(), static_cast<std::streamsize>(source.size()));
		}

		TempSource(const TempSource&) = delete;
		TempSource& operator=(const TempSource&) = delete;

		~TempSource()
		{
			std::error_code ignored;
			std::filesystem::remove(_path, ignored);
		}

		const std::filesystem::path& path() const noexcept { return _path; }
		std::string string() const { return _path.string(); }
	};

	// main calls double_it twice, so there is a real call stack to reconstruct, and a variable in
	// .data to watch change.
	//  1 @data
	//  2     let total: u32 = 0
	//  3 @text
	//  4 global main:
	//  5     li r1, 5
	//  6     call double_it
	//  7     stv r0, total
	//  8     li r0, 1
	//  9     out 0xff, r0
	// 10     ret
	// 11
	// 12 double_it:
	// 13     add r0, r1, r1
	// 14     ret
	constexpr std::string_view CallSource =
		"@data\r\n"
		"    let total: u32 = 0\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 5\r\n"
		"    call double_it\r\n"
		"    stv r0, total\r\n"
		"    li r0, 1\r\n"
		"    out 0xff, r0\r\n"
		"    ret\r\n"
		"\r\n"
		"double_it:\r\n"
		"    add r0, r1, r1\r\n"
		"    ret\r\n";

	constexpr std::string_view LaSource =
		"@data\r\n"
		"    let total: u32 = 0\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 5\r\n"
		"    la r1, total\r\n"
		"    li r0, 1\r\n"
		"    out 0xff, r0\r\n"
		"    ret\r\n";

	std::unique_ptr<debug::DebugSession> launchOrNull(const TempSource& source, bool stopOnEntry = true)
	{
		auto session = debug::DebugSession::launch(debug::LaunchConfig{
			.sources = { source.path() },
			.memorySize = vm::Memory::DefaultSize,
			.stopOnEntry = stopOnEntry
		});

		if (!session.has_value())
		{
			Registry::instance().recordFailure(session.error());
			return nullptr;
		}
		return std::move(session.value());
	}
}

TEST(debugger, a_session_starts_at_the_entry_point_without_running_anything)
{
	TempSource source{ CallSource, "entry" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	const debug::StopEvent event = session->start();

	CHECK(event.reason == debug::StopReason::Entry);
	CHECK_EQ(event.address, TextStart);
	CHECK_EQ(session->registers().executedInstructions, u64{ 0 });

	const auto location = session->currentLocation();
	CHECK(location.has_value());
	if (location.has_value())
		CHECK_EQ(location->expansionLine, 5u);
}

TEST(debugger, stepping_one_instruction_advances_exactly_one_word)
{
	TempSource source{ CallSource, "stepi" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	const debug::StopEvent event = session->stepInstruction();

	CHECK(event.reason == debug::StopReason::Step);
	CHECK_EQ(event.address, TextStart + vm::Instruction::Size);
	CHECK_EQ(session->registers().executedInstructions, u64{ 1 });
	CHECK_EQ(session->registers().general[1], 5u);
}

TEST(debugger, a_line_breakpoint_stops_on_the_line_it_names)
{
	TempSource source{ CallSource, "bpline" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	// Line 13 is `add r0, r1, r1`, inside double_it.
	const auto id = session->addLineBreakpoint(source.string(), 13);
	CHECK(id.has_value());
	if (!id.has_value()) { Registry::instance().recordFailure(id.error()); return; }

	session->start();
	const debug::StopEvent event = session->resume();

	CHECK(event.reason == debug::StopReason::Breakpoint);
	CHECK_EQ(event.breakpoint, id.value());

	const auto location = session->currentLocation();
	CHECK(location.has_value());
	if (location.has_value())
		CHECK_EQ(location->expansionLine, 13u);

	CHECK_EQ(session->breakpoints().front().hitCount, 1u);
}

TEST(debugger, a_breakpoint_on_a_line_with_no_code_is_refused_with_a_reason)
{
	TempSource source{ CallSource, "bpempty" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	// Line 11 is blank, so nothing was emitted for it and there is nowhere to stop.
	const auto id = session->addLineBreakpoint(source.string(), 11);
	CHECK(!id.has_value());
	CHECK(session->breakpoints().empty());
}

TEST(debugger, a_symbol_breakpoint_stops_at_the_start_of_the_subroutine)
{
	TempSource source{ CallSource, "bpsym" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	const auto id = session->addSymbolBreakpoint("double_it");
	CHECK(id.has_value());
	if (!id.has_value()) { Registry::instance().recordFailure(id.error()); return; }

	session->start();
	const debug::StopEvent event = session->resume();

	CHECK(event.reason == debug::StopReason::Breakpoint);

	const auto location = session->currentLocation();
	CHECK(location.has_value());
	if (location.has_value())
		CHECK_EQ(location->expansionLine, 13u);

	// A variable is not a place to stop.
	CHECK(!session->addSymbolBreakpoint("total").has_value());
	CHECK(!session->addSymbolBreakpoint("no_such_label").has_value());
}

TEST(debugger, the_call_stack_grows_on_a_call_and_shrinks_on_the_return)
{
	TempSource source{ CallSource, "stack" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	CHECK_EQ(session->callStack().size(), usize{ 1 });
	CHECK_EQ(std::string(session->callStack().back().name), std::string("main"));

	session->addSymbolBreakpoint("double_it");
	session->resume();

	CHECK_EQ(session->callStack().size(), usize{ 2 });
	CHECK_EQ(std::string(session->callStack().back().name), std::string("double_it"));
	// The frame remembers where it will go back to, which is the instruction after the CALL.
	CHECK(session->callStack().back().returnAddress > TextStart);
	CHECK(session->callStack().back().reconstructed);

	// Run the two instructions of double_it; the RET pops the frame.
	session->stepInstruction();
	session->stepInstruction();
	CHECK_EQ(session->callStack().size(), usize{ 1 });
	CHECK_EQ(std::string(session->callStack().back().name), std::string("main"));
}

TEST(debugger, stepping_over_a_call_runs_it_to_completion)
{
	TempSource source{ CallSource, "stepover" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	session->stepLine(); // line 5 -> line 6, the call

	const auto beforeLocation = session->currentLocation();
	CHECK(beforeLocation.has_value());
	if (beforeLocation.has_value())
		CHECK_EQ(beforeLocation->expansionLine, 6u);

	const debug::StopEvent event = session->stepOver();

	CHECK(event.reason == debug::StopReason::Step);
	// Landed on line 7, not inside double_it, and the call did happen: r0 is 10.
	const auto after = session->currentLocation();
	CHECK(after.has_value());
	if (after.has_value())
		CHECK_EQ(after->expansionLine, 7u);
	CHECK_EQ(session->callStack().size(), usize{ 1 });
	CHECK_EQ(session->registers().general[0], 10u);
}

TEST(debugger, stepping_into_a_call_enters_it_and_stepping_out_returns)
{
	TempSource source{ CallSource, "stepinout" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	session->stepLine(); // -> line 6
	session->stepLine(); // -> into double_it, line 13

	const auto inside = session->currentLocation();
	CHECK(inside.has_value());
	if (inside.has_value())
		CHECK_EQ(inside->expansionLine, 13u);
	CHECK_EQ(session->callStack().size(), usize{ 2 });

	const debug::StopEvent event = session->stepOut();
	CHECK(event.reason == debug::StopReason::Step);
	CHECK_EQ(session->callStack().size(), usize{ 1 });

	const auto back = session->currentLocation();
	CHECK(back.has_value());
	if (back.has_value())
		CHECK_EQ(back->expansionLine, 7u);
}

TEST(debugger, a_pseudo_instruction_is_one_step_even_though_it_is_several_words)
{
	// `stv` used to be the example here, at three words. It relaxes to one now that the
	// variable is close enough to reach, so the multi-word case has to be something that
	// still is one: `la` is always lui + ori.
	TempSource source{ LaSource, "steppseudo" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	session->addLineBreakpoint(source.string(), 6); // la r1, total - lui + ori
	session->resume();

	const u64 before = session->registers().executedInstructions;
	const debug::StopEvent event = session->stepLine();
	const u64 after = session->registers().executedInstructions;

	CHECK(event.reason == debug::StopReason::Step);
	// Two instructions retired, but the user saw one step and landed on the next line.
	CHECK_EQ(after - before, u64{ 2 });

	const auto location = session->currentLocation();
	CHECK(location.has_value());
	if (location.has_value())
		CHECK_EQ(location->expansionLine, 7u);
}

TEST(debugger, running_to_the_end_reports_the_program_exiting)
{
	TempSource source{ CallSource, "exit" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	const debug::StopEvent event = session->resume();

	CHECK(event.reason == debug::StopReason::Exited);
	CHECK(!session->isRunning());
}

TEST(debugger, the_program_output_reaches_the_handler_byte_by_byte)
{
	constexpr std::string_view PrintSource =
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 72\r\n"
		"    outb 0x01, r1\r\n"
		"    li r1, 105\r\n"
		"    outb 0x01, r1\r\n"
		"    li r0, 1\r\n"
		"    out 0xff, r0\r\n"
		"    ret\r\n";

	TempSource source{ PrintSource, "output" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	std::string captured;
	session->setOutputHandler([&captured](std::span<const u8> bytes)
	{
		for (u8 byte : bytes)
			captured.push_back(static_cast<char>(byte));
	});

	session->start();
	session->resume();

	CHECK_EQ(captured, std::string("Hi"));
}

TEST(debugger, a_fault_reports_the_instruction_that_caused_it_not_the_handler)
{
	// r1 = 0x401 is not a multiple of four, so the word load raises AlignmentFault.
	constexpr std::string_view FaultSource =
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 0x401\r\n"
		"    ldr r2, [r1 + 0]\r\n"
		"    ret\r\n";

	TempSource source{ FaultSource, "fault" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	const debug::StopEvent event = session->resume();

	CHECK(event.reason == debug::StopReason::Exception);
	CHECK(event.exception == vm::InterruptNumber::AlignmentFault);
	// The load is the second instruction, so the fault belongs to it - not to the BIOS handler
	// the program counter has by now jumped to.
	CHECK_EQ(event.exceptionAddress, TextStart + vm::Instruction::Size);
	CHECK(event.address != event.exceptionAddress);

	// The handler is a frame of its own, with the faulting code still underneath it.
	CHECK_EQ(session->callStack().size(), usize{ 2 });
	CHECK(session->callStack().back().isInterruptHandler);
}

TEST(debugger, an_endless_loop_comes_back_as_a_step_limit_rather_than_hanging)
{
	constexpr std::string_view LoopSource =
		"@text\r\n"
		"global main:\r\n"
		".spin:\r\n"
		"    jp .spin\r\n";

	TempSource source{ LoopSource, "spin" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	const debug::StopEvent event = session->resume(5000);

	CHECK(event.reason == debug::StopReason::StepLimit);
	CHECK_EQ(session->registers().executedInstructions, u64{ 5000 });
}

TEST(debugger, registers_and_memory_can_be_written_from_outside)
{
	TempSource source{ CallSource, "write" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();

	CHECK(session->setRegister("r7", 0xABCD1234));
	CHECK_EQ(session->registers().general[7], 0xABCD1234u);

	CHECK(session->setRegister("sp", 0x00800000));
	CHECK_EQ(session->registers().general[vm::GeneralPurposeRegisterPool::StackPointerIndex], 0x00800000u);

	CHECK(!session->setRegister("r99", 1));
	CHECK(!session->setRegister("nonsense", 1));

	// The program counter has to stay on an instruction boundary, or the next fetch decodes
	// whatever straddles two words.
	CHECK(session->setProgramCounter(TextStart + 8));
	CHECK_EQ(session->programCounter(), TextStart + 8);
	CHECK(!session->setProgramCounter(TextStart + 1));

	const std::array<u8, 4> bytes{ 0xDE, 0xAD, 0xBE, 0xEF };
	CHECK(session->writeMemory(TextStart + 0x1000, bytes));
	const std::vector<u8> readBack = session->readMemory(TextStart + 0x1000, 4);
	CHECK_EQ(readBack.size(), usize{ 4 });
	if (readBack.size() == 4)
		CHECK_EQ(readBack[0], u8{ 0xDE });
}

TEST(debugger, globals_are_rendered_through_the_types_the_assembler_recorded)
{
	constexpr std::string_view VarSource =
		"const LIMIT = 42\r\n"
		"@rodata\r\n"
		"    let greeting: u8[5] = \"hola\"\r\n"
		"@data\r\n"
		"    let scores: i16[3] = [1, -2, 3]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n";

	TempSource source{ VarSource, "globals" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	const auto globals = session->globals();

	const auto find = [&](std::string_view name) -> const debug::VariableView*
	{
		for (const auto& variable : globals)
		{
			if (variable.name == name)
				return &variable;
		}
		return nullptr;
	};

	const debug::VariableView* greeting = find("greeting");
	CHECK(greeting != nullptr);
	if (greeting != nullptr)
	{
		// A u8 array is a string in this language far more often than it is six numbers.
		CHECK_EQ(greeting->value, std::string("\"hola\""));
		CHECK_EQ(greeting->type, std::string("u8[5]"));
	}

	const debug::VariableView* scores = find("scores");
	CHECK(scores != nullptr);
	if (scores != nullptr)
	{
		CHECK_EQ(scores->value, std::string("[1, -2, 3]"));
		CHECK_EQ(scores->type, std::string("i16[3]"));
	}

	const debug::VariableView* limit = find("LIMIT");
	CHECK(limit != nullptr);
	if (limit != nullptr)
	{
		CHECK(limit->isConstant);
		CHECK_EQ(limit->value, std::string("42"));
	}
}

TEST(debugger, a_variable_reflects_what_the_program_has_written_into_it)
{
	TempSource source{ CallSource, "livevar" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();

	const auto totalBefore = session->globals();
	const auto findTotal = [](const std::vector<debug::VariableView>& all) -> std::string
	{
		for (const auto& variable : all)
		{
			if (variable.name == "total")
				return variable.value;
		}
		return "<missing>";
	};

	CHECK_EQ(findTotal(totalBefore), std::string("0"));

	// Line 8 is after the `stv r0, total` on line 7 has run.
	session->addLineBreakpoint(source.string(), 8);
	session->resume();

	CHECK_EQ(findTotal(session->globals()), std::string("10"));
}

TEST(debugger, restarting_puts_the_machine_back_at_the_entry_point)
{
	TempSource source{ CallSource, "restart" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	session->resume();
	CHECK(!session->isRunning());

	const debug::StopEvent event = session->restart();

	CHECK(event.reason == debug::StopReason::Entry);
	CHECK_EQ(event.address, TextStart);
	CHECK_EQ(session->registers().executedInstructions, u64{ 0 });
	CHECK_EQ(session->registers().general[1], 0u);
	CHECK(session->isRunning());
}

TEST(debugger, a_program_with_no_debug_information_still_steps)
{
	// Everything that needs the line table degrades to addresses; nothing refuses to work.
	TempSource source{ CallSource, "nodebug" };

	auto assembled = [&]() -> std::optional<std::filesystem::path>
	{
		casm::Assembler assembler{};
		auto program = assembler.assemble({ source.path() });
		if (!program.has_value())
			return std::nullopt;

		const auto path = std::filesystem::temp_directory_path() / "ceres_dbg_nodebug.cres";
		if (!program->saveToFile(path))
			return std::nullopt;
		return path;
	}();

	CHECK(assembled.has_value());
	if (!assembled.has_value()) return;

	auto session = debug::DebugSession::launch(debug::LaunchConfig{ .sources = { assembled.value() } });
	CHECK(session.has_value());
	if (!session.has_value()) { Registry::instance().recordFailure(session.error()); return; }

	auto& debugger = *session.value();
	CHECK(debugger.debugInfo().isEmpty());

	debugger.start();
	CHECK(!debugger.currentLocation().has_value());

	// Stepping a line with no line table means stepping one instruction, which is the most useful
	// thing it can mean.
	const debug::StopEvent event = debugger.stepLine();
	CHECK(event.reason == debug::StopReason::Step);
	CHECK_EQ(event.address, TextStart + vm::Instruction::Size);

	// And a line breakpoint says why it cannot be set rather than silently doing nothing.
	CHECK(!debugger.addLineBreakpoint(source.string(), 5).has_value());

	std::error_code ignored;
	std::filesystem::remove(assembled.value(), ignored);
}

TEST(debugger, the_disassembly_reads_backwards_as_well_as_forwards)
{
	TempSource source{ CallSource, "disasm" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	session->addSymbolBreakpoint("double_it");
	session->resume();

	const u32 pc = session->programCounter();
	const auto lines = session->disassemble(pc, 2, 3);

	CHECK_EQ(lines.size(), usize{ 5 });
	if (lines.size() == 5)
	{
		// Every instruction is four bytes, so walking backwards is exact rather than a guess.
		CHECK_EQ(lines[0].address, pc - 8);
		CHECK_EQ(lines[2].address, pc);
		CHECK_EQ(lines[4].address, pc + 8);
		CHECK_EQ(lines[2].symbol, std::string("double_it"));
		CHECK(lines[2].location.has_value());
	}
}

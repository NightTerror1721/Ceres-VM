// The expression evaluator, and the three things built on it: conditional breakpoints, logpoints
// and hit counts. Plus watchpoints, which are the one debugger feature that watches memory rather
// than the program counter.

#include "framework.h"
#include "debug/debug_session.h"
#include "debug/expression.h"
#include "vm/memory.h"
#include <filesystem>
#include <fstream>

using namespace ceres;
using namespace ceres::testing;

namespace
{
	constexpr u32 TextStart = static_cast<u32>(vm::Memory::UnrestrictedSegmentStartValue);

	class TempSource
	{
	private:
		std::filesystem::path _path;

	public:
		TempSource(std::string_view source, std::string_view stem)
		{
			_path = std::filesystem::temp_directory_path() / std::format("ceres_expr_{}.casm", stem);
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

	//  1 const LIMIT = 10
	//  2 @data
	//  3     let counter: u32 = 0
	//  4     let scores: i16[3] = [10, -20, 30]
	//  5 @rodata
	//  6     let name: u8[5] = "hola"
	//  7 @text
	//  8 global main:
	//  9     li r3, 0
	// 10 .loop:
	// 11     add r3, r3, 1
	// 12     stv r3, counter
	// 13     cmp r3, LIMIT
	// 14     jnz .loop
	// 15     li r0, 1
	// 16     out 0xff, r0
	// 17     ret
	constexpr std::string_view LoopSource =
		"const LIMIT = 10\r\n"
		"@data\r\n"
		"    let counter: u32 = 0\r\n"
		"    let scores: i16[3] = [10, -20, 30]\r\n"
		"@rodata\r\n"
		"    let name: u8[5] = \"hola\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r3, 0\r\n"
		".loop:\r\n"
		"    add r3, r3, 1\r\n"
		"    stv r3, counter\r\n"
		"    cmp r3, LIMIT\r\n"
		"    jnz .loop\r\n"
		"    li r0, 1\r\n"
		"    out 0xff, r0\r\n"
		"    ret\r\n";

	std::unique_ptr<debug::DebugSession> launchOrNull(const TempSource& source)
	{
		auto session = debug::DebugSession::launch(debug::LaunchConfig{ .sources = { source.path() } });
		if (!session.has_value())
		{
			Registry::instance().recordFailure(session.error());
			return nullptr;
		}
		return std::move(session.value());
	}

	// Runs to the point where r3 has reached `value`, so an expression has something to read.
	std::unique_ptr<debug::DebugSession> runToCounter(const TempSource& source, u32 value)
	{
		auto session = launchOrNull(source);
		if (!session)
			return nullptr;

		session->start();
		session->addLineBreakpoint(source.string(), 13, { .condition = std::format("r3 == {}", value) });
		session->resume();
		return session;
	}

	std::string valueOf(const debug::DebugSession& session, std::string_view expression)
	{
		auto result = session.evaluate(expression);
		return result.has_value() ? result->text : std::format("<error: {}>", result.error());
	}
}

TEST(expression, registers_and_flags_read_as_numbers)
{
	TempSource source{ LoopSource, "regs" };
	auto session = runToCounter(source, 4);
	CHECK(session != nullptr);
	if (!session) return;

	CHECK_EQ(valueOf(*session, "r3"), std::string("4"));
	CHECK_EQ(valueOf(*session, "pc"), std::format("{}", session->programCounter()));
	CHECK_EQ(valueOf(*session, "ticks"), std::format("{}", session->registers().executedInstructions));

	// A flag is 0 or 1, which is what makes it usable in a condition without special syntax.
	const std::string zero = valueOf(*session, "zero");
	CHECK(zero == "0" || zero == "1");

	CHECK(!session->evaluate("r99").has_value());
	CHECK(!session->evaluate("nonsense").has_value());
}

TEST(expression, arithmetic_follows_the_precedence_you_would_expect)
{
	TempSource source{ LoopSource, "arith" };
	auto session = runToCounter(source, 4);
	CHECK(session != nullptr);
	if (!session) return;

	CHECK_EQ(valueOf(*session, "1 + 2 * 3"), std::string("7"));
	CHECK_EQ(valueOf(*session, "(1 + 2) * 3"), std::string("9"));
	CHECK_EQ(valueOf(*session, "10 - 2 - 3"), std::string("5"));
	CHECK_EQ(valueOf(*session, "7 % 3"), std::string("1"));
	CHECK_EQ(valueOf(*session, "1 << 8"), std::string("256"));
	CHECK_EQ(valueOf(*session, "0xff & 0x0f"), std::string("15"));
	CHECK_EQ(valueOf(*session, "0b1010"), std::string("10"));
	CHECK_EQ(valueOf(*session, "-5 + 8"), std::string("3"));
	CHECK_EQ(valueOf(*session, "~0 & 0xff"), std::string("255"));

	// Comparison binds looser than arithmetic, or `r3 + 1 == 5` would parse as `r3 + (1 == 5)`.
	CHECK_EQ(valueOf(*session, "r3 + 1 == 5"), std::string("1"));
	CHECK_EQ(valueOf(*session, "r3 > 10"), std::string("0"));
	CHECK_EQ(valueOf(*session, "r3 > 1 && r3 < 10"), std::string("1"));
	CHECK_EQ(valueOf(*session, "r3 == 4 || r3 == 9"), std::string("1"));
	CHECK_EQ(valueOf(*session, "!0"), std::string("1"));

	// A shift is not two comparisons, however similar they look.
	CHECK_EQ(valueOf(*session, "2 << 2"), std::string("8"));
	CHECK_EQ(valueOf(*session, "16 >> 2"), std::string("4"));

	CHECK(!session->evaluate("1 / 0").has_value());
	CHECK(!session->evaluate("1 +").has_value());
	CHECK(!session->evaluate("(1 + 2").has_value());
	CHECK(!session->evaluate("").has_value());
}

TEST(expression, symbols_read_through_the_types_the_assembler_recorded)
{
	TempSource source{ LoopSource, "symbols" };
	auto session = runToCounter(source, 4);
	CHECK(session != nullptr);
	if (!session) return;

	// A constant carries its value; a scalar variable is read from live memory.
	CHECK_EQ(valueOf(*session, "LIMIT"), std::string("10"));
	CHECK_EQ(valueOf(*session, "counter"), std::string("4"));

	// A label is its address, so `pc == main` is a thing you can ask.
	CHECK_EQ(valueOf(*session, "main"), std::format("{}", TextStart));

	// Indexing uses the element type, not bytes: scores is i16[3], and the second element is
	// negative, which only comes out right if the load sign-extends.
	CHECK_EQ(valueOf(*session, "scores[0]"), std::string("10"));
	CHECK_EQ(valueOf(*session, "scores[1]"), std::string("-20"));
	CHECK_EQ(valueOf(*session, "scores[2]"), std::string("30"));
	CHECK_EQ(valueOf(*session, "scores[1] + scores[2]"), std::string("10"));

	// A u8 array renders as the string it almost always is.
	CHECK_EQ(valueOf(*session, "name"), std::string("\"hola\""));
	CHECK_EQ(valueOf(*session, "name[0]"), std::string("104")); // 'h'
}

TEST(expression, memory_can_be_read_directly_and_by_type)
{
	TempSource source{ LoopSource, "memory" };
	auto session = runToCounter(source, 4);
	CHECK(session != nullptr);
	if (!session) return;

	const auto counterSymbol = session->debugInfo().symbolNamed("counter");
	CHECK(counterSymbol != nullptr);
	if (counterSymbol == nullptr) return;

	// A bare [addr] is a word, the size the machine loads by default.
	CHECK_EQ(valueOf(*session, std::format("[{}]", counterSymbol->address)), std::string("4"));
	CHECK_EQ(valueOf(*session, std::format("u8[{}]", counterSymbol->address)), std::string("4"));
	CHECK_EQ(valueOf(*session, std::format("u32[{}]", counterSymbol->address)), std::string("4"));

	// An address expression, not just a literal - which is the form that makes [r1 + 4] useful.
	session->setRegister("r1", counterSymbol->address);
	CHECK_EQ(valueOf(*session, "[r1 + 0]"), std::string("4"));
	CHECK_EQ(valueOf(*session, "u8[r1]"), std::string("4"));

	// Past the end of memory is an error, not a zero.
	CHECK(!session->evaluate("[0xFFFFFFF0]").has_value());
	// A type on its own is not a value.
	CHECK(!session->evaluate("u8").has_value());
}

TEST(expression, a_message_has_its_expressions_interpolated)
{
	TempSource source{ LoopSource, "interpolate" };
	auto session = runToCounter(source, 4);
	CHECK(session != nullptr);
	if (!session) return;

	CHECK_EQ(debug::interpolate(*session, "r3 is {r3}"), std::string("r3 is 4"));
	CHECK_EQ(debug::interpolate(*session, "{counter} of {LIMIT}"), std::string("4 of 10"));
	CHECK_EQ(debug::interpolate(*session, "no expressions here"), std::string("no expressions here"));

	// An unmatched brace is text, not a malformed expression.
	CHECK_EQ(debug::interpolate(*session, "a { b"), std::string("a { b"));

	// A failing expression is reported in place: a logpoint that silently stops logging would be
	// worse than one that says what went wrong.
	const std::string broken = debug::interpolate(*session, "x={nope}");
	CHECK(broken.starts_with("x=<"));
	CHECK(broken.ends_with(">"));
}

TEST(expression, a_conditional_breakpoint_stops_only_when_it_holds)
{
	TempSource source{ LoopSource, "conditional" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	const auto id = session->addLineBreakpoint(source.string(), 13, { .condition = "r3 == 7" });
	CHECK(id.has_value());
	if (!id.has_value()) return;

	const debug::StopEvent event = session->resume();

	CHECK(event.reason == debug::StopReason::Breakpoint);
	CHECK_EQ(valueOf(*session, "r3"), std::string("7"));
	// The loop passed that line six times before, and none of those count: a hit is a hit the
	// condition allowed through.
	CHECK_EQ(session->breakpoints().front().hitCount, 1u);
}

TEST(expression, a_condition_that_cannot_be_evaluated_stops_and_says_so)
{
	TempSource source{ LoopSource, "badcondition" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	std::string logged;
	session->setLogHandler([&logged](std::string_view text) { logged += text; });

	session->start();
	session->addLineBreakpoint(source.string(), 13, { .condition = "no_such_symbol == 1" });

	// Silently never firing would leave the user watching a breakpoint with no clue why.
	const debug::StopEvent event = session->resume();
	CHECK(event.reason == debug::StopReason::Breakpoint);
	CHECK(logged.find("could not be evaluated") != std::string::npos);
}

TEST(expression, a_hit_count_counts_the_hits_the_condition_allowed_through)
{
	TempSource source{ LoopSource, "hitcount" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	// Every third arrival, so the third pass through the loop.
	session->addLineBreakpoint(source.string(), 13, { .hitCondition = "%3" });
	session->resume();

	CHECK_EQ(valueOf(*session, "r3"), std::string("3"));
	CHECK_EQ(session->breakpoints().front().hitCount, 3u);

	session->resume();
	CHECK_EQ(valueOf(*session, "r3"), std::string("6"));

	// ">=5" from a fresh session, to check the other shape.
	auto second = launchOrNull(source);
	CHECK(second != nullptr);
	if (!second) return;
	second->start();
	second->addLineBreakpoint(source.string(), 13, { .hitCondition = ">=5" });
	second->resume();
	CHECK_EQ(valueOf(*second, "r3"), std::string("5"));
}

TEST(expression, a_logpoint_reports_and_carries_on)
{
	TempSource source{ LoopSource, "logpoint" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	std::vector<std::string> logged;
	session->setLogHandler([&logged](std::string_view text) { logged.emplace_back(text); });

	session->start();
	session->addLineBreakpoint(source.string(), 13, { .logMessage = "r3={r3}" });

	// A logpoint is not a breakpoint: the program runs to the end.
	const debug::StopEvent event = session->resume();

	CHECK(event.reason == debug::StopReason::Exited);
	CHECK_EQ(logged.size(), usize{ 10 });
	if (logged.size() == 10)
	{
		CHECK_EQ(logged.front(), std::string("r3=1"));
		CHECK_EQ(logged.back(), std::string("r3=10"));
	}
}

TEST(expression, a_watchpoint_stops_when_the_memory_it_watches_changes)
{
	TempSource source{ LoopSource, "watch" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();

	const auto counterSymbol = session->debugInfo().symbolNamed("counter");
	CHECK(counterSymbol != nullptr);
	if (counterSymbol == nullptr) return;

	const auto id = session->addDataBreakpoint(counterSymbol->address, counterSymbol->size, "counter");
	CHECK(id.has_value());
	if (!id.has_value()) return;

	const debug::StopEvent event = session->resume();

	CHECK(event.reason == debug::StopReason::DataBreakpoint);
	CHECK_EQ(event.dataBreakpoint, id.value());
	// The store on line 12 is what changed it, so by now counter holds 1.
	CHECK_EQ(valueOf(*session, "counter"), std::string("1"));
	CHECK_EQ(session->dataBreakpoints().front().hitCount, 1u);

	// And again on the next iteration, rather than firing once and going quiet.
	session->resume();
	CHECK_EQ(valueOf(*session, "counter"), std::string("2"));

	CHECK(session->removeDataBreakpoint(id.value()));
	CHECK(session->dataBreakpoints().empty());
	CHECK(!session->addDataBreakpoint(0, 0, "empty").has_value());
}

TEST(expression, exception_filters_choose_which_faults_stop_the_machine)
{
	constexpr std::string_view FaultSource =
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 0x401\r\n"
		"    ldr r2, [r1 + 0]\r\n"
		"    ret\r\n";

	TempSource source{ FaultSource, "filters" };

	// By default every system exception stops the machine.
	{
		auto session = launchOrNull(source);
		CHECK(session != nullptr);
		if (!session) return;

		session->start();
		CHECK(session->stopsOn(vm::InterruptNumber::AlignmentFault));
		CHECK(session->resume().reason == debug::StopReason::Exception);
	}

	// Narrowed to something else, the alignment fault goes to its handler unremarked and the
	// machine carries on into the BIOS stub, which halts.
	{
		auto session = launchOrNull(source);
		CHECK(session != nullptr);
		if (!session) return;

		session->setExceptionFilters(std::vector<vm::InterruptNumber>{ vm::InterruptNumber::DivisionByZero });
		CHECK(!session->stopsOn(vm::InterruptNumber::AlignmentFault));
		CHECK(session->stopsOn(vm::InterruptNumber::DivisionByZero));

		session->start();
		const debug::StopEvent event = session->resume(20000);
		CHECK(event.reason != debug::StopReason::Exception);
	}
}

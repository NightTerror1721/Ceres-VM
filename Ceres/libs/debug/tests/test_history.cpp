// Running the machine backwards, and counting where it has been.
//
// The claim these check is a strong one: going back and coming forward again lands on exactly the
// same machine, byte for byte. That is only possible because the machine is deterministic - the
// timer counts executed instructions rather than wall clock - and it is worth pinning, because a
// reverse debugger that is *almost* right is worse than none.

#include "framework.h"
#include <ceres/debug/debug_session.h>
#include <ceres/core/format/memory_map.h>
#include <filesystem>
#include <fstream>

using namespace ceres;
using namespace ceres::testing;

namespace
{
	class TempSource
	{
	private:
		std::filesystem::path _path;

	public:
		TempSource(std::string_view source, std::string_view stem)
		{
			_path = std::filesystem::temp_directory_path() / std::format("ceres_hist_{}.casm", stem);
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

	//  1 @data
	//  2     let counter: u32 = 0
	//  3 @text
	//  4 global main:
	//  5     li r3, 0
	//  6 .loop:
	//  7     add r3, r3, 1
	//  8     stv r3, counter
	//  9     cmp r3, 2000
	// 10     jnz .loop
	// 11     li r0, 1
	// 12     la r13, 0xFFFF0000   (SystemControlDevice's MMIO base)
	// 13     strb [r13 + 0], r0
	// 14     ret
	constexpr std::string_view LongLoop =
		"@data\r\n"
		"    let counter: u32 = 0\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r3, 0\r\n"
		".loop:\r\n"
		"    add r3, r3, 1\r\n"
		"    stv counter, r3\r\n"
		"    cmp r3, 2000\r\n"
		"    jnz .loop\r\n"
		"    li r0, 1\r\n"
		"    la r13, 0xFFFF0000\r\n"
		"    strb [r13 + 0], r0\r\n"
		"    ret\r\n";

	std::unique_ptr<debug::DebugSession> launchOrNull(const TempSource& source, bool record = true)
	{
		auto session = debug::DebugSession::launch(debug::LaunchConfig{
			.sources = { source.path() },
			.recordHistory = record,
			// Small enough that a test reaches several snapshots without running for a second.
			.history = { .interval = 200, .maxSnapshots = 64 }
		});

		if (!session.has_value())
		{
			Registry::instance().recordFailure(session.error());
			return nullptr;
		}
		return std::move(session.value());
	}

	// Everything that makes one moment in the machine's life different from another.
	struct Fingerprint
	{
		std::vector<u8> memory;
		std::array<u32, vm::GeneralPurposeRegisterPool::Count> registers{};
		u32 flags = 0;
		u32 programCounter = 0;
		u64 ticks = 0;

		bool operator==(const Fingerprint&) const = default;
	};

	Fingerprint fingerprint(const debug::DebugSession& session)
	{
		Fingerprint out;
		const debug::RegisterView view = session.registers();
		out.registers = view.general;
		out.flags = view.flags;
		out.programCounter = view.programCounter;
		out.ticks = view.executedInstructions;
		// The whole machine, not a sample of it: a delta-encoded snapshot that restored all but
		// one page would pass any narrower check.
		out.memory = session.readMemory(0, static_cast<u32>(vm::Memory::DefaultSize));
		return out;
	}
}

TEST(history, going_back_and_forward_again_restores_the_machine_byte_for_byte)
{
	TempSource source{ LongLoop, "roundtrip" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	session->resume(6000);

	const u64 target = session->currentTick();
	CHECK(target > 1000);

	const Fingerprint before = fingerprint(*session);

	// Back a thousand instructions, then forward the same thousand.
	const debug::StopEvent back = session->runToTick(target - 1000);
	CHECK(back.reason == debug::StopReason::Step);
	CHECK_EQ(session->currentTick(), target - 1000);

	const Fingerprint middle = fingerprint(*session);
	CHECK(!(middle == before));

	session->resume(1000);
	CHECK_EQ(session->currentTick(), target);

	const Fingerprint after = fingerprint(*session);
	CHECK(after == before);
}

TEST(history, a_hundred_thousand_instructions_rewind_a_thousand_and_come_back_identical)
{
	// The same property as above at the scale it was promised at, and with the real snapshot
	// interval rather than the small one the other tests use - so this exercises restoring a
	// snapshot and replaying nineteen thousand instructions to reach the moment asked for.
	constexpr std::string_view BigLoop =
		"@data\r\n"
		"    let counter: u32 = 0\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r3, 0\r\n"
		".loop:\r\n"
		"    add r3, r3, 1\r\n"
		"    stv counter, r3\r\n"
		"    cmp r3, 30000\r\n"
		"    jnz .loop\r\n"
		"    li r0, 1\r\n"
		"    la r13, 0xFFFF0000\r\n"
		"    strb [r13 + 0], r0\r\n"
		"    ret\r\n";

	TempSource source{ BigLoop, "hundredthousand" };
	auto session = debug::DebugSession::launch(debug::LaunchConfig{ .sources = { source.path() } });
	CHECK(session.has_value());
	if (!session.has_value()) { Registry::instance().recordFailure(session.error()); return; }

	auto& debugger = *session.value();
	debugger.start();
	debugger.resume(100'000);

	CHECK_EQ(debugger.currentTick(), u64{ 100'000 });
	const Fingerprint before = fingerprint(debugger);

	CHECK(debugger.runToTick(99'000).reason == debug::StopReason::Step);
	CHECK_EQ(debugger.currentTick(), u64{ 99'000 });

	debugger.resume(1'000);
	CHECK_EQ(debugger.currentTick(), u64{ 100'000 });
	CHECK(fingerprint(debugger) == before);
}

TEST(history, the_same_moment_is_the_same_machine_however_you_reach_it)
{
	TempSource source{ LongLoop, "converge" };

	// Two sessions of the same program, one run straight there and one that went back and forward
	// again, have to agree about everything.
	auto straight = launchOrNull(source);
	auto detoured = launchOrNull(source);
	CHECK(straight != nullptr);
	CHECK(detoured != nullptr);
	if (!straight || !detoured) return;

	straight->start();
	straight->resume(3000);

	detoured->start();
	detoured->resume(5000);
	detoured->runToTick(3000);

	CHECK_EQ(straight->currentTick(), u64{ 3000 });
	CHECK_EQ(detoured->currentTick(), u64{ 3000 });
	CHECK(fingerprint(*straight) == fingerprint(*detoured));
}

TEST(history, stepping_back_one_instruction_undoes_exactly_one)
{
	TempSource source{ LongLoop, "stepback" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	session->resume(500);

	const Fingerprint before = fingerprint(*session);
	const u64 tick = session->currentTick();

	const debug::StopEvent back = session->stepBackInstruction();
	CHECK(back.reason == debug::StopReason::Step);
	CHECK_EQ(session->currentTick(), tick - 1);

	// And one forward puts it back exactly.
	session->stepInstruction();
	CHECK(fingerprint(*session) == before);
}

TEST(history, stepping_back_a_line_lands_on_a_different_line)
{
	TempSource source{ LongLoop, "stepbackline" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	session->resume(500);

	// Land on the head of a line first, so "the line before this one" is well defined.
	session->stepLine();
	const auto here = session->currentLocation();
	CHECK(here.has_value());
	if (!here.has_value()) return;

	const debug::StopEvent back = session->stepBackLine();
	CHECK(back.reason == debug::StopReason::Step);

	const auto there = session->currentLocation();
	CHECK(there.has_value());
	if (there.has_value())
	{
		CHECK(there->expansionLine != here->expansionLine);
		// And on the head of that line, not somewhere inside it.
		CHECK((there->flags & debug::LineFlag::FirstOfLine) != 0);
	}
}

TEST(history, reverse_continue_finds_the_previous_time_a_breakpoint_was_hit)
{
	TempSource source{ LongLoop, "reverse" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	session->addLineBreakpoint(source.string(), 7);

	// Forward to the fourth arrival at line 7. The breakpoint fires *before* the add on that
	// line, so on the fourth arrival r3 still holds the three increments before it.
	for (int i = 0; i < 4; ++i)
		session->resume();

	const u32 forward = session->registers().general[3];
	CHECK_EQ(forward, 3u);

	const debug::StopEvent back = session->reverseContinue();
	CHECK(back.reason == debug::StopReason::Breakpoint);
	CHECK_EQ(session->registers().general[3], 2u);

	session->reverseContinue();
	CHECK_EQ(session->registers().general[3], 1u);
}

TEST(history, the_recording_reaches_only_as_far_back_as_it_keeps)
{
	TempSource source{ LongLoop, "window" };
	auto session = debug::DebugSession::launch(debug::LaunchConfig{
		.sources = { source.path() },
		.recordHistory = true,
		// Deliberately tiny: three snapshots two hundred instructions apart.
		.history = { .interval = 200, .maxSnapshots = 3 }
	});
	CHECK(session.has_value());
	if (!session.has_value()) { Registry::instance().recordFailure(session.error()); return; }

	auto& debugger = *session.value();
	debugger.start();
	debugger.resume(5000);

	CHECK(debugger.history().snapshotCount() <= usize{ 3 });
	CHECK(debugger.history().oldestTick() > 0);

	// Anything older than the window says so instead of quietly landing somewhere else.
	const debug::StopEvent tooFar = debugger.runToTick(1);
	CHECK(tooFar.reason == debug::StopReason::Error);
	CHECK(tooFar.message.find("further back") != std::string::npos);

	// Within it, it works.
	const u64 reachable = debugger.history().oldestTick();
	CHECK(debugger.runToTick(reachable).reason == debug::StopReason::Step);
	CHECK_EQ(debugger.currentTick(), reachable);
}

TEST(history, a_session_without_recording_says_so_rather_than_pretending)
{
	TempSource source{ LongLoop, "norecord" };
	auto session = launchOrNull(source, false);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	session->resume(500);

	CHECK(!session->history().isEnabled());
	CHECK(!session->canStepBack());

	const debug::StopEvent back = session->stepBackInstruction();
	CHECK(back.reason == debug::StopReason::Error);
	CHECK(back.message.find("without recording") != std::string::npos);
}

TEST(history, snapshots_cost_the_pages_a_program_actually_touches)
{
	TempSource source{ LongLoop, "cost" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	const usize atStart = session->history().memoryCost();
	session->resume(6000);
	const usize afterRunning = session->history().memoryCost();

	// The base image dominates; the snapshots on top of it are deltas of the working set, which
	// for this program is one page of code and one of data.
	const usize snapshotCost = afterRunning - atStart;
	const usize snapshots = session->history().snapshotCount();
	CHECK(snapshots > 1);
	if (snapshots > 1)
	{
		// Comfortably under a full copy of memory per snapshot, which is the whole point of
		// storing them as page deltas.
		CHECK(snapshotCost < snapshots * 64 * 1024);
	}
}

TEST(history, coverage_counts_every_word_and_reports_the_ones_never_reached)
{
	// Two branches, one of which is never taken, so a zero in the coverage means something.
	constexpr std::string_view BranchSource =
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 1\r\n"
		"    cmp r1, 1\r\n"
		"    jnz .never\r\n"
		"    li r2, 10\r\n"
		"    jp .done\r\n"
		".never:\r\n"
		"    li r2, 20\r\n"
		".done:\r\n"
		"    li r0, 1\r\n"
		"    la r13, 0xFFFF0000\r\n"
		"    strb [r13 + 0], r0\r\n"
		"    ret\r\n";

	TempSource source{ BranchSource, "coverage" };
	auto session = launchOrNull(source);
	CHECK(session != nullptr);
	if (!session) return;

	session->start();
	session->resume();

	const auto entries = session->coverage();
	CHECK(!entries.empty());

	u64 reached = 0;
	u64 never = 0;
	std::optional<u64> neverLine;

	for (const debug::CoverageEntry& entry : entries)
	{
		if (entry.count > 0)
		{
			++reached;
			continue;
		}

		++never;
		// Line 9 is the branch nothing takes.
		if (entry.location.has_value() && entry.location->expansionLine == 9)
			neverLine = entry.location->expansionLine;
	}

	CHECK(reached > 0);
	CHECK(never > 0);
	CHECK(neverLine.has_value());

	// And the entry point ran exactly once.
	const auto first = std::ranges::find(entries, static_cast<u32>(vm::Memory::UnrestrictedSegmentStartValue),
		&debug::CoverageEntry::address);
	CHECK(first != entries.end());
	if (first != entries.end())
		CHECK_EQ(first->count, u64{ 1 });
}

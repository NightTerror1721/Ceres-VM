// The line and symbol tables: the assembler used to compute where every source line ended up and
// then throw it away when assemble() returned. These pin the two properties everything downstream
// depends on — that the mapping round-trips in both directions, and that a .cres carrying it is
// still the same file to anything that does not care.

#include "framework.h"
#include "assemble_helper.h"
#include "debug/debug_info.h"
#include "vm/memory.h"
#include "vm/ceresvm.h"
#include "vm/program.h"
#include <sstream>

using namespace ceres;
using namespace ceres::testing;

namespace
{
	constexpr u32 TextStart = static_cast<u32>(vm::Memory::UnrestrictedSegmentStartValue);

	// Line 1 is `@rodata`, so the first instruction is on line 5.
	constexpr std::string_view SampleSource =
		"@rodata\r\n"
		"    let msg: u8[4] = \"abc\"\r\n"
		"const LIMIT = 7\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 1\r\n"
		"    la r2, msg\r\n"
		"    add r1, r1, r1\r\n"
		"    ret\r\n";

	// Every instruction in the expansion of `twice` belongs, as far as a debugger is concerned, to
	// the line that wrote `twice r1` — not to the two lines of the macro's body.
	constexpr std::string_view MacroSource =
		"macro twice $reg\r\n"
		"    add $reg, $reg, $reg\r\n"
		"    add $reg, $reg, $reg\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 3\r\n"
		"    twice r1\r\n"
		"    ret\r\n";
}

TEST(debuginfo, nothing_is_recorded_unless_it_is_asked_for)
{
	AssembleResult assembled = assembleSource(SampleSource, "dbg_off");

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	CHECK(assembled.debugInfo.isEmpty());
	CHECK(!assembled.program->hasDebugSection());
	CHECK_EQ(assembled.program->header().flags & vm::ProgramFlags::HasDebugInfo, u16{ 0 });
}

TEST(debuginfo, every_instruction_line_round_trips_to_its_own_address)
{
	AssembleResult assembled = assembleSource(SampleSource, "dbg_roundtrip", true);

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	const debug::DebugInfo& info = assembled.debugInfo;
	CHECK(!info.isEmpty());

	// The property the whole table exists for: asking for a line's addresses and then asking each
	// of those addresses where it came from has to land back on the same line.
	for (const debug::LineEntry& entry : info.lines())
	{
		const auto addresses = info.addressesOf(assembled.sourcePath, entry.expansionLine);
		CHECK(!addresses.empty());

		for (u32 address : addresses)
		{
			const auto location = info.locationOf(address);
			CHECK(location.has_value());
			if (location.has_value())
				CHECK_EQ(location->expansionLine, entry.expansionLine);
		}
	}
}

TEST(debuginfo, a_pseudo_instruction_occupies_several_addresses_but_is_entered_at_the_first)
{
	AssembleResult assembled = assembleSource(SampleSource, "dbg_pseudo", true);

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	const debug::DebugInfo& info = assembled.debugInfo;

	// `li r1, 1` on line 6, then `la r2, msg` on line 7 expanding to LUI + ORI.
	const auto liAddresses = info.addressesOf(assembled.sourcePath, 6);
	const auto laAddresses = info.addressesOf(assembled.sourcePath, 7);

	CHECK_EQ(liAddresses.size(), usize{ 1 });
	CHECK_EQ(laAddresses.size(), usize{ 2 });

	// A breakpoint on line 7 belongs on the LUI, not on the ORI that finishes the address.
	const auto first = info.firstAddressOfLine(assembled.sourcePath, 7);
	CHECK(first.has_value());
	if (first.has_value())
	{
		CHECK_EQ(first.value(), TextStart + 4u);
		CHECK_EQ(first.value(), laAddresses.front());
	}
}

TEST(debuginfo, an_address_inside_an_instruction_resolves_to_that_instruction)
{
	AssembleResult assembled = assembleSource(SampleSource, "dbg_inside", true);

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	const auto exact = assembled.debugInfo.locationOf(TextStart);
	const auto inside = assembled.debugInfo.locationOf(TextStart + 2);
	CHECK(exact.has_value());
	CHECK(inside.has_value());
	if (exact.has_value() && inside.has_value())
		CHECK_EQ(inside->expansionLine, exact->expansionLine);

	// Past the end of .text is not "inside the last instruction".
	CHECK(!assembled.debugInfo.locationOf(TextStart + 4096).has_value());
	CHECK(!assembled.debugInfo.locationOf(0).has_value());
}

TEST(debuginfo, macro_expanded_instructions_point_at_the_call_site)
{
	AssembleResult assembled = assembleSource(MacroSource, "dbg_macro", true);

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	const debug::DebugInfo& info = assembled.debugInfo;

	// `twice r1` is on line 8 and produces two ADDs. Both belong to line 8 as far as stepping is
	// concerned, while still recording lines 2 and 3 as where they were actually written.
	const auto addresses = info.addressesOf(assembled.sourcePath, 8);
	CHECK_EQ(addresses.size(), usize{ 2 });

	for (u32 address : addresses)
	{
		const auto location = info.locationOf(address);
		CHECK(location.has_value());
		if (!location.has_value())
			continue;

		CHECK_EQ(location->expansionLine, 8u);
		CHECK(location->isMacroExpansion());
		CHECK_EQ(location->macroDepth, u16{ 1 });
		// Where it was written, which is inside the macro body.
		CHECK(location->line == 2u || location->line == 3u);
	}

	// The macro's own body lines are not places a breakpoint can be set: nothing was emitted
	// *at* them, only from them.
	CHECK(info.addressesOf(assembled.sourcePath, 2).empty());
}

TEST(debuginfo, symbols_carry_addresses_types_and_constant_values)
{
	AssembleResult assembled = assembleSource(SampleSource, "dbg_symbols", true);

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	const debug::DebugInfo& info = assembled.debugInfo;

	const debug::SymbolEntry* main = info.symbolNamed("main");
	CHECK(main != nullptr);
	if (main != nullptr)
	{
		CHECK_EQ(main->kind, static_cast<u8>(debug::SymbolKind::Label));
		CHECK_EQ(main->address, TextStart);
		CHECK_EQ(main->flags & debug::SymbolFlag::Global, u8{ debug::SymbolFlag::Global });
	}

	const debug::SymbolEntry* msg = info.symbolNamed("msg");
	CHECK(msg != nullptr);
	if (msg != nullptr)
	{
		CHECK_EQ(msg->kind, static_cast<u8>(debug::SymbolKind::Variable));
		CHECK_EQ(msg->section, static_cast<u8>(debug::SymbolSection::Rodata));
		CHECK_EQ(msg->scalarType, static_cast<u8>(debug::ScalarType::U8));
		CHECK_EQ(msg->elementCount, 4u);
		CHECK_EQ(msg->size, 4u);
	}

	// A constant occupies no memory, so it has no section and no address — but it does have the
	// value a watch expression will want.
	const debug::SymbolEntry* limit = info.symbolNamed("LIMIT");
	CHECK(limit != nullptr);
	if (limit != nullptr)
	{
		CHECK_EQ(limit->kind, static_cast<u8>(debug::SymbolKind::Constant));
		CHECK_EQ(limit->section, static_cast<u8>(debug::SymbolSection::None));
		CHECK_EQ(limit->flags & debug::SymbolFlag::HasValue, u8{ debug::SymbolFlag::HasValue });
		CHECK_EQ(limit->value, 7u);
	}

	// `msg` starts the rodata section, so an address inside it resolves to it and one past its
	// four bytes does not.
	CHECK(info.symbolContaining(msg->address) == msg);
	CHECK(info.symbolContaining(msg->address + 3) == msg);
	CHECK(info.symbolContaining(msg->address + 4) != msg);
}

TEST(debuginfo, the_tables_survive_serialisation)
{
	AssembleResult assembled = assembleSource(SampleSource, "dbg_serialise", true);

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	const std::vector<u8> bytes = assembled.debugInfo.serialize();
	CHECK(!bytes.empty());

	auto reloaded = debug::DebugInfo::deserialize(bytes);
	CHECK(reloaded.has_value());
	if (!reloaded.has_value()) { Registry::instance().recordFailure(reloaded.error()); return; }

	CHECK_EQ(reloaded->lines().size(), assembled.debugInfo.lines().size());
	CHECK_EQ(reloaded->symbols().size(), assembled.debugInfo.symbols().size());
	CHECK_EQ(reloaded->fileCount(), assembled.debugInfo.fileCount());

	// The queries, not just the byte counts: a table that survives as bytes but answers
	// differently is no use.
	CHECK_EQ(reloaded->addressesOf(assembled.sourcePath, 7).size(), usize{ 2 });

	const debug::SymbolEntry* limit = reloaded->symbolNamed("LIMIT");
	CHECK(limit != nullptr);
	if (limit != nullptr)
		CHECK_EQ(limit->value, 7u);

	// Serialising what came back has to produce the same bytes, or the format is not canonical
	// and two builds of the same source could differ.
	CHECK(reloaded->serialize() == bytes);
}

TEST(debuginfo, a_truncated_or_foreign_debug_section_is_rejected_rather_than_misread)
{
	AssembleResult assembled = assembleSource(SampleSource, "dbg_truncated", true);

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	std::vector<u8> bytes = assembled.debugInfo.serialize();

	std::vector<u8> truncated(bytes.begin(), bytes.begin() + bytes.size() / 2);
	CHECK(!debug::DebugInfo::deserialize(truncated).has_value());

	std::vector<u8> wrongMagic = bytes;
	wrongMagic[0] = 0x00;
	CHECK(!debug::DebugInfo::deserialize(wrongMagic).has_value());

	CHECK(!debug::DebugInfo::deserialize(std::span<const u8>{}).has_value());
}

TEST(debuginfo, a_cres_carries_its_debug_section_through_a_round_trip)
{
	AssembleResult assembled = assembleSource(SampleSource, "dbg_cres", true);

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	CHECK(assembled.program->hasDebugSection());
	CHECK_EQ(assembled.program->header().flags & vm::ProgramFlags::HasDebugInfo,
		u16{ vm::ProgramFlags::HasDebugInfo });

	std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
	const auto written = assembled.program->writeToStream(stream);
	CHECK(written.has_value());
	if (!written.has_value()) { Registry::instance().recordFailure(written.error()); return; }

	auto reloaded = vm::Program::loadFromStream(stream);
	CHECK(reloaded.has_value());
	if (!reloaded.has_value()) { Registry::instance().recordFailure(reloaded.error()); return; }

	CHECK(reloaded->hasDebugSection());
	CHECK(std::ranges::equal(reloaded->text(), assembled.program->text()));

	auto info = debug::DebugInfo::deserialize(reloaded->debugSection());
	CHECK(info.has_value());
	if (info.has_value())
		CHECK_EQ(info->addressesOf(assembled.sourcePath, 7).size(), usize{ 2 });
}

TEST(debuginfo, adding_a_debug_section_leaves_the_program_bytes_untouched)
{
	// The compatibility promise: every loader written before this existed reads exactly the sizes
	// the header declares and stops, so what it sees has to be byte-for-byte what it saw before.
	AssembleResult plain = assembleSource(SampleSource, "dbg_compat_plain", false);
	AssembleResult withDebug = assembleSource(SampleSource, "dbg_compat_debug", true);

	CHECK(plain.ok());
	CHECK(withDebug.ok());
	if (!plain.ok() || !withDebug.ok()) { Registry::instance().recordFailure("assembly failed"); return; }

	CHECK(std::ranges::equal(plain.program->text(), withDebug.program->text()));
	CHECK(std::ranges::equal(plain.program->rodata(), withDebug.program->rodata()));
	CHECK(std::ranges::equal(plain.program->data(), withDebug.program->data()));
	CHECK_EQ(plain.program->header().entryPoint, withDebug.program->header().entryPoint);
	CHECK_EQ(plain.program->header().textSize, withDebug.program->header().textSize);

	// And on disk: the debug bytes all sit past the end of what an older reader would consume.
	std::stringstream plainStream(std::ios::in | std::ios::out | std::ios::binary);
	std::stringstream debugStream(std::ios::in | std::ios::out | std::ios::binary);
	CHECK(plain.program->writeToStream(plainStream).has_value());
	CHECK(withDebug.program->writeToStream(debugStream).has_value());

	const std::string plainBytes = plainStream.str();
	const std::string debugBytes = debugStream.str();

	CHECK(debugBytes.size() > plainBytes.size());
	// Everything after the flags field, up to where the plain file ends, is identical; the flags
	// word itself is the one difference, and an older reader ignores bits it does not know.
	CHECK(plainBytes.compare(8, std::string::npos, debugBytes, 8, plainBytes.size() - 8) == 0);
}

TEST(debuginfo, a_file_is_found_by_an_absolute_path_a_relative_one_or_its_name)
{
	AssembleResult assembled = assembleSource(SampleSource, "dbg_paths", true);

	CHECK(assembled.ok());
	if (!assembled.ok()) { Registry::instance().recordFailure(assembled.joinedErrors()); return; }

	const debug::DebugInfo& info = assembled.debugInfo;

	// Exactly as recorded.
	CHECK(!info.addressesOf(assembled.sourcePath, 6).empty());

	// The same path with a redundant component, which is what a normalising editor may send.
	const std::filesystem::path awkward =
		std::filesystem::path(assembled.sourcePath).parent_path() / "." /
		std::filesystem::path(assembled.sourcePath).filename();
	CHECK(!info.addressesOf(awkward.string(), 6).empty());

	// The bare file name, which is unambiguous here because only one file was assembled.
	CHECK(!info.addressesOf(std::filesystem::path(assembled.sourcePath).filename().string(), 6).empty());

	CHECK(info.addressesOf("no_such_file.casm", 6).empty());
}

// --- Profiling ---------------------------------------------------------------------------------

TEST(debug_info, the_machine_counts_how_often_each_instruction_runs)
{
	// The counters are indexed off the text range, so they only mean anything once a program has
	// been loaded and the range is known.
	AssembleResult a = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 3\r\n"
		".loop:\r\n"
		"    dec r1\r\n"
		"    ifne r1, 0, .loop\r\n"
		"    ret\r\n", "profile");

	CHECK(a.ok());
	if (!a.ok()) { Registry::instance().recordFailure(a.joinedErrors()); return; }

	vm::CeresVM machine{};
	CHECK(machine.loadProgram(a.program.value()).has_value());
	machine.engine().enableProfiling();

	for (int i = 0; i < 12; ++i)
		machine.engine().step();

	const auto counts = machine.engine().executionCounts();
	CHECK_EQ(counts.size(), a.program->header().textSize / vm::Instruction::Size);
	if (counts.empty()) return;

	// The li runs once and the body of the loop more than once.
	CHECK_EQ(counts[0], u64{ 1 });
	CHECK(counts[1] > 1);
}

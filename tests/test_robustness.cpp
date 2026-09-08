// Phase 3: things that used to be undefined behaviour or silent truncation should now be
// diagnosed. Each case here failed, crashed or lied before the fix it names.

#include "framework.h"
#include "assemble_helper.h"
#include "vm/ceresvm.h"
#include "vm/bios.h"
#include "vm/memory.h"
#include <string>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::testing;

namespace
{
	class Machine
	{
	private:
		CeresVM _vm;

	public:
		explicit Machine(std::initializer_list<Instruction> program, usize memorySize = Memory::DefaultSize)
			: _vm(memorySize)
		{
			const Address entry = Memory::UnrestrictedSegmentStart;

			usize offset = 0;
			for (Instruction instruction : program)
			{
				_vm.memory().writeUnchecked<u32>(entry + Address(static_cast<u32>(offset)), instruction.raw());
				offset += Instruction::Size;
			}

			// Without the BIOS vectors every fault finds a null handler and is ignored, which is
			// precisely what these tests check does not happen.
			BIOS bios{};
			bios.initializeMemory(_vm.memory());

			_vm.memory().writeUnchecked<u32>(0_addr, entry.value());
			_vm.engine().reset();
		}

		void step(usize count = 1)
		{
			for (usize i = 0; i < count; ++i)
				_vm.engine().step();
		}

		u32 reg(usize index) const { return _vm.engine().registers().getValue(index); }

		const FlagRegister& flags() const { return _vm.engine().flags(); }
		Address pc() const { return _vm.engine().programCounter(); }
		Memory& memory() { return _vm.memory(); }
	};
}

// --- VM-01: the dispatch table left 145 null entries ---------------------------------------

TEST(robustness, an_unmapped_opcode_traps_instead_of_crashing)
{
	// 0xEE is not assigned. Before the fix this called a null member function pointer and took
	// the process down; now it raises IllegalInstruction, whose BIOS vector halts the machine.
	Machine m{ Instruction(0xEE000000u) };
	m.step();

	CHECK(m.flags().halting() || m.pc().value() >= Memory::BiosSegmentStart.value());
}

TEST(robustness, fneg_is_reachable)
{
	// FNEG had a handler and an opcode but was never registered in the dispatch table, so it
	// was one of the null entries.
	Machine m{
		Instruction::LI(1, 5),
		Instruction::ITOF(0, 1),
		Instruction::FNEG(2, 0),
	};
	m.step(3);

	CHECK(!m.flags().halting());
	CHECK_EQ(m.pc().value(), Memory::UnrestrictedSegmentStart.value() + 3 * Instruction::Size);
}

// --- VM-05: two factories emitted the wrong opcode -----------------------------------------

TEST(robustness, the_byte_and_halfword_port_factories_emit_their_own_opcodes)
{
	CHECK(Instruction::INRB(1, 2).opcode() == Opcode::INRB);
	CHECK(Instruction::INRH(1, 2).opcode() == Opcode::INRH);
}

// --- Stack bounds ----------------------------------------------------------------------------

TEST(robustness, an_unbalanced_ret_does_not_read_past_the_stack)
{
	// RET with nothing pushed pops from the very top of memory. That read used to run off the
	// end; now it raises the stack fault.
	Machine m{ Instruction::RET() };
	m.step();

	CHECK(m.flags().halting() || m.flags().trap() || m.pc().value() >= Memory::BiosSegmentStart.value());
}

TEST(robustness, pushing_past_the_restricted_segment_faults_instead_of_writing_into_it)
{
	// SP is set explicitly just above the boundary rather than by letting a loop drain the
	// stack: a runaway stack overwrites the program text long before it reaches 0x400, because
	// nothing tells the machine where the loaded image ends. This guard protects the interrupt
	// vectors and the BIOS, which is what it claims to do.
	Machine m({
		Instruction::LI(1, 0x404),
		Instruction::MOV(15, 1),      // SP = 0x404, one word above the restricted segment
		Instruction::PUSH(1),         // fits: SP becomes 0x400
		Instruction::PUSH(1),         // does not fit: must fault
	}, 8192);

	m.step(4);

	// The fault is taken rather than the push happening. It dispatches now instead of stopping
	// the machine: the handler has a stack of its own to save state on, which is the whole point
	// of reserving one.
	CHECK(m.pc().value() >= Memory::BiosSegmentStart.value());
	CHECK(m.pc().value() < Memory::UnrestrictedSegmentStart.value());

	// Nothing was written through the null page: the reset vector still holds the entry point.
	CHECK_EQ(m.memory().readUnchecked<u32>(0_addr), Memory::UnrestrictedSegmentStart.value());
}

TEST(robustness, a_push_that_still_fits_is_not_faulted)
{
	// The boundary check must not be off by one: a push landing exactly on the first
	// unrestricted address is legal.
	Machine m({
		Instruction::LI(1, 0x404),
		Instruction::MOV(15, 1),
		Instruction::PUSH(1),
	}, 8192);

	m.step(3);

	CHECK(!m.flags().halting());
	CHECK(!m.flags().trap());
	CHECK_EQ(m.reg(15), 0x400u);
}

// --- SE-16: immediates that do not fit are rejected, not truncated -------------------------

TEST(robustness, an_immediate_too_wide_for_its_field_is_rejected)
{
	AssembleResult r = assembleSource(inText("    li r0, 70000"));

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("does not fit") != std::string::npos);
}

TEST(robustness, a_negative_immediate_still_fits_a_16_bit_field)
{
	// -1 is 0xFFFFFFFF, which fits 16 bits when read as signed. Rejecting it would have been
	// over-strict.
	AssembleResult r = assembleSource(inText("    li r0, -1"));

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 0xFFFF });
}

TEST(robustness, a_port_number_wider_than_eight_bits_is_rejected)
{
	AssembleResult r = assembleSource(inText("    out 0x1FF, r1"));

	CHECK(!r.ok());
}

// --- SE-07: a declaration larger than its initialiser is padded ----------------------------

// The guard in the emitter against a section coming out shorter than the space reserved for it
// used to be unreachable: the parser demanded that an initialiser fill its declaration exactly.
// Now a declared size pads, which is the same rule that lets an inner array dimension pad a short
// row - so this exercises the guard instead of pinning it shut.
TEST(robustness, a_declaration_longer_than_its_initialiser_is_padded)
{
	AssembleResult r = assembleSource(
		"@rodata\r\n"
		"    let head: u8[16] = \"hi\"\r\n"
		"    let tail: u8[4] = \"ab\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto rodata = r.program->rodata();
	CHECK_EQ(rodata.size(), usize{ 20 });
	if (rodata.size() < 20) return;

	CHECK_EQ(static_cast<char>(rodata[0]), 'h');
	CHECK_EQ(static_cast<char>(rodata[1]), 'i');
	CHECK_EQ(static_cast<u32>(rodata[2]), u32{ 0 });  // the string's own terminator
	CHECK_EQ(static_cast<u32>(rodata[15]), u32{ 0 }); // and the padding out to 16
	CHECK_EQ(static_cast<char>(rodata[16]), 'a');     // the next variable starts where it should
}

TEST(robustness, an_initialiser_longer_than_its_declaration_is_still_rejected)
{
	AssembleResult r = assembleSource(
		"@rodata\r\n"
		"    let head: u8[2] = \"hello\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
}

TEST(robustness, the_header_sizes_match_the_emitted_buffers)
{
	AssembleResult r = assembleSource(
		"@rodata\r\n"
		"    let label: u8[6] = \"short\"\r\n"
		"@data\r\n"
		"    let counter: u32 = 7\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto& header = r.program->header();
	CHECK_EQ(r.program->text().size(), usize{ header.textSize });
	CHECK_EQ(r.program->rodata().size(), usize{ header.rodataSize });
	CHECK_EQ(r.program->data().size(), usize{ header.dataSize });
}

// --- SE-15: literal comparison must not read indeterminate union bytes ---------------------

TEST(robustness, narrow_literals_compare_by_value_not_by_raw_union)
{
	const auto a = casm::LiteralScalar::makeU8(7);
	const auto b = casm::LiteralScalar::makeU8(7);
	const auto c = casm::LiteralScalar::makeU8(8);
	const auto wide = casm::LiteralScalar::makeU32(7);

	CHECK(a == b);
	CHECK(!(a == c));
	CHECK(!(a == wide));           // same value, different declared width
	CHECK_EQ(a.rawBits(), 7u);
	CHECK_EQ(casm::LiteralScalar::makeI8(-1).rawBits(), 0xFFFFFFFFu);
}

// --- Diagnostics -----------------------------------------------------------------------------

TEST(robustness, an_error_is_reported_on_the_line_it_occurs_on)
{
	// Token line numbers were read after the character had been consumed, so anything anchored
	// to an end-of-line token landed one line late.
	AssembleResult r = assembleSource(
		"@text\r\n"          // 1
		"global main:\r\n"   // 2
		"    nop\r\n"        // 3
		"    li r0, 70000\r\n" // 4  <- the error
		"    ret\r\n");      // 5

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("[line 4]") != std::string::npos);
}

TEST(robustness, an_unresolved_symbol_names_itself_readably)
{
	// std::format used to select the range formatter for interned strings and print
	// ['n', 'o', 'p', 'e'] instead of nope.
	AssembleResult r = assembleSource(inText("    jp nope"));

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("nope") != std::string::npos);
	CHECK(r.joinedErrors().find("'n', 'o'") == std::string::npos);
}

// --- SE-09: linking must be reproducible ----------------------------------------------------

TEST(robustness, assembling_the_same_source_twice_produces_identical_output)
{
	const std::string source =
		"@rodata\r\n"
		"    let msg: u8[4] = \"abc\"\r\n"
		"@data\r\n"
		"    let n: u32 = 1\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    la r1, msg\r\n"
		"    ldv r2, n\r\n"
		"    ret\r\n";

	AssembleResult first = assembleSource(source, "repro_a");
	AssembleResult second = assembleSource(source, "repro_b");

	CHECK(first.ok());
	CHECK(second.ok());
	if (!first.ok() || !second.ok()) return;

	// words() returns by value; calling it twice in one expression compares iterators into
	// two separate temporaries, both already destroyed.
	const std::vector<u32> firstWords = first.words();
	const std::vector<u32> secondWords = second.words();

	CHECK_EQ(firstWords.size(), secondWords.size());
	CHECK(firstWords == secondWords);
	CHECK_EQ(first.program->header().entryPoint, second.program->header().entryPoint);
}

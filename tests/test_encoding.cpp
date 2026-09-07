// Encoding tests: source text in, machine words out.
//
// These are the tests that would have caught every Phase 1 defect. Each one pins a specific
// field placement, so a regression shows up as a disassembly mismatch rather than as a program
// that quietly does the wrong thing.

#include "framework.h"
#include "assemble_helper.h"
#include "vm/memory.h"

using namespace ceres;
using namespace ceres::testing;

namespace
{
	// Builds the expected word the same way the VM's own factories do, so an expectation can
	// never drift from the encoding the machine actually decodes.
	using vm::Instruction;
	using vm::Opcode;

	std::vector<u32> assembleText(std::string_view body, AssembleResult& out)
	{
		out = assembleSource(inText(body));
		return out.words();
	}
}

TEST(encoding, li_places_destination_and_immediate)
{
	AssembleResult r;
	auto words = assembleText("    li r3, 1234", r);

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(words.size(), 1u);
	CHECK_EQ_FMT(words[0], static_cast<u32>(Instruction::LI(3, 1234)), renderWord);
}

TEST(encoding, add_with_immediate_selects_addi)
{
	AssembleResult r;
	auto words = assembleText("    add r1, r2, 7", r);

	CHECK(r.ok());
	CHECK_EQ(words.size(), 1u);
	CHECK_EQ_FMT(words[0], static_cast<u32>(Instruction::ADDI(1, 2, 7)), renderWord);
}

TEST(encoding, add_with_three_registers_selects_add)
{
	AssembleResult r;
	auto words = assembleText("    add r1, r2, r3", r);

	CHECK(r.ok());
	CHECK_EQ_FMT(words[0], static_cast<u32>(Instruction::ADD(1, 2, 3)), renderWord);
}

// --- SE-03: the displacement of a base+offset access must reach the encoding ---------------

TEST(encoding, load_encodes_a_nonzero_displacement)
{
	AssembleResult r;
	auto words = assembleText("    ldrb r2, [r1 + 12]", r);

	CHECK(r.ok());
	CHECK_EQ_FMT(words[0], static_cast<u32>(Instruction::LDRB(2, 1, 12)), renderWord);

	// The specific regression: imm16 silently coming out as zero.
	CHECK_EQ(Instruction(words[0]).imm16(), u16{ 12 });
}

TEST(encoding, load_without_displacement_encodes_zero)
{
	AssembleResult r;
	auto words = assembleText("    ldrb r2, [r1]", r);

	CHECK(r.ok());
	CHECK_EQ(Instruction(words[0]).imm16(), u16{ 0 });
	CHECK_EQ(Instruction(words[0]).rs(), u8{ 1 });
	CHECK_EQ(Instruction(words[0]).rd(), u8{ 2 });
}

// --- SE-04: stores put the base in Rd and the value in Rs; Rt overlaps imm16 ---------------

TEST(encoding, store_puts_base_in_rd_and_value_in_rs)
{
	AssembleResult r;
	auto words = assembleText("    strb r6, [r5 + 1]", r);

	CHECK(r.ok());

	const Instruction encoded{ words[0] };
	CHECK_EQ(encoded.opcode() == Opcode::STRB, true);
	CHECK_EQ(encoded.rd(), u8{ 5 });     // base
	CHECK_EQ(encoded.rs(), u8{ 6 });     // value
	CHECK_EQ(encoded.imm16(), u16{ 1 });
	CHECK_EQ_FMT(words[0], static_cast<u32>(Instruction::STRB(5, 6, 1)), renderWord);
}

TEST(encoding, store_displacement_does_not_collide_with_the_value_register)
{
	// A displacement whose high nibble is non-zero would have been read back as the value
	// register index under the old encoding.
	AssembleResult r;
	auto words = assembleText("    str r2, [r1 + 61680]", r);

	CHECK(r.ok());
	const Instruction encoded{ words[0] };
	CHECK_EQ(encoded.imm16(), u16{ 61680 });   // 0xF0F0
	CHECK_EQ(encoded.rs(), u8{ 2 });
	CHECK_EQ(encoded.rd(), u8{ 1 });
}

// --- SE-05: a port held in a register must be encoded as a register ------------------------

TEST(encoding, out_with_immediate_port_uses_imm8)
{
	AssembleResult r;
	auto words = assembleText("    out 0x01, r5", r);

	CHECK(r.ok());
	CHECK_EQ_FMT(words[0], static_cast<u32>(Instruction::OUT(5, 0x01)), renderWord);
}

TEST(encoding, out_with_register_port_selects_outr)
{
	AssembleResult r;
	auto words = assembleText("    out r8, r9", r);

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	const Instruction encoded{ words[0] };
	CHECK_EQ(encoded.opcode() == Opcode::OUTR, true);
	CHECK_EQ(encoded.rt(), u8{ 8 });   // port
	CHECK_EQ(encoded.rs(), u8{ 9 });   // value
}

// --- SE-02 / SE-06: LA and STV materialise a 32-bit address in two halves ------------------

TEST(encoding, la_splits_the_address_into_lui_and_ori)
{
	AssembleResult r = assembleSource(
		"@rodata\r\n"
		"    let msg: u8[4] = \"abc\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    la r1, msg\r\n");
	auto words = r.words();

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(words.size(), 2u);

	const Instruction upper{ words[0] };
	const Instruction lower{ words[1] };

	CHECK_EQ(upper.opcode() == Opcode::LUI, true);
	CHECK_EQ(lower.opcode() == Opcode::ORI, true);

	// The halves must recombine into the symbol's real address. Under the SE-02 bug LUI always
	// carried zero, which stayed invisible while addresses fit in 16 bits.
	const u32 rebuilt = (static_cast<u32>(upper.imm16()) << 16) | lower.imm16();
	const u32 rodataStart = vm::Memory::UnrestrictedSegmentStart.value() + r.program->header().textSize;
	CHECK_EQ(rebuilt, rodataStart);

	// ORI must read from the destination register, not from some other operand.
	CHECK_EQ(lower.rd(), u8{ 1 });
	CHECK_EQ(lower.rs(), u8{ 1 });
}

TEST(encoding, stv_does_not_mix_the_data_register_into_the_address)
{
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    let counter: u32 = 0\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 5\r\n"
		"    stv r1, counter\r\n");
	auto words = r.words();

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(words.size(), 4u);   // LI + LUI + ORI + STR

	const Instruction upper{ words[1] };
	const Instruction lower{ words[2] };
	const Instruction store{ words[3] };

	CHECK_EQ(upper.opcode() == Opcode::LUI, true);
	CHECK_EQ(lower.opcode() == Opcode::ORI, true);
	CHECK_EQ(store.opcode() == Opcode::STR, true);

	// The scratch register must feed itself, never the user's data register (SE-06).
	CHECK_EQ(lower.rd(), lower.rs());
	CHECK(lower.rd() != 1);

	// And it must not be R13, which is declared as the Link Register.
	CHECK(lower.rd() != 13);

	// The store reads its base from the scratch register and its value from the user's.
	CHECK_EQ(store.rd(), upper.rd());
	CHECK_EQ(store.rs(), u8{ 1 });
}

// --- Branches are PC-relative to the branch itself -----------------------------------------

TEST(encoding, backward_jump_encodes_a_negative_displacement)
{
	AssembleResult r;
	auto words = assembleText(
		"    nop\r\n"
		".loop:\r\n"
		"    nop\r\n"
		"    jp .loop", r);

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(words.size(), 3u);

	// The jump sits at word 2 and targets word 1: one instruction backwards.
	const i32 displacement = Instruction(words[2]).simm24().signedValue();
	CHECK_EQ(displacement, -static_cast<i32>(vm::Instruction::Size));
}

TEST(encoding, forward_call_resolves_to_a_positive_displacement)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    call target\r\n"
		"    halt\r\n"
		"target:\r\n"
		"    ret\r\n");
	auto words = r.words();

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	const i32 displacement = Instruction(words[0]).simm24().signedValue();
	CHECK_EQ(displacement, 2 * static_cast<i32>(vm::Instruction::Size));
}

// --- SE-19: integer literals are untyped until the declaration types them ------------------

TEST(encoding, narrow_array_literals_are_stored_at_the_declared_width)
{
	AssembleResult r = assembleSource(
		"@rodata\r\n"
		"    let magic: i16[4] = [42, -10, 0x1A, 0b10]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto rodata = r.program->rodata();
	CHECK_EQ(rodata.size(), usize{ 8 });   // four halfwords, not four words

	const auto readI16 = [&](usize index) {
		return static_cast<i16>(static_cast<u16>(rodata[index * 2]) |
			(static_cast<u16>(rodata[index * 2 + 1]) << 8));
	};

	CHECK_EQ(readI16(0), i16{ 42 });
	CHECK_EQ(readI16(1), i16{ -10 });
	CHECK_EQ(readI16(2), i16{ 26 });
	CHECK_EQ(readI16(3), i16{ 2 });
}

TEST(encoding, an_out_of_range_literal_is_rejected)
{
	AssembleResult r = assembleSource(
		"@rodata\r\n"
		"    let overflows: u16[2] = [1, 70000]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("does not fit") != std::string::npos);
}

TEST(encoding, a_float_literal_is_rejected_where_an_integer_is_declared)
{
	AssembleResult r = assembleSource(
		"@rodata\r\n"
		"    let mixed: i16[2] = [1, 3.5]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
}

// --- SE-20: INM and OUTM take their operands in the same order -----------------------------

TEST(encoding, outm_takes_port_address_size_like_inm)
{
	AssembleResult r;
	auto words = assembleText("    outm 0x01, r3, r2", r);

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	const Instruction encoded{ words[0] };
	CHECK_EQ(encoded.opcode() == Opcode::OUTM, true);
	CHECK_EQ(encoded.imm8(), u8{ 0x01 });   // port
	CHECK_EQ(encoded.rs(), u8{ 3 });        // address
	CHECK_EQ(encoded.rt(), u8{ 2 });        // size
}

TEST(encoding, inm_operand_order_is_unchanged)
{
	AssembleResult r;
	auto words = assembleText("    inm 0x02, r3, r2", r);

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	const Instruction encoded{ words[0] };
	CHECK_EQ(encoded.opcode() == Opcode::INM, true);
	CHECK_EQ(encoded.imm8(), u8{ 0x02 });   // port
	CHECK_EQ(encoded.rd(), u8{ 3 });        // address
	CHECK_EQ(encoded.rs(), u8{ 2 });        // size
}

// --- SE-01: no phantom byte in front of string literals ------------------------------------

TEST(encoding, string_literals_have_no_leading_padding_byte)
{
	AssembleResult r = assembleSource(
		"@rodata\r\n"
		"    let msg: u8[6] = \"Hello\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto rodata = r.program->rodata();

	// The section is padded to a 4-byte boundary, so six declared bytes occupy eight. What
	// matters here is that the string starts at offset zero, with no phantom byte in front.
	CHECK(rodata.size() >= 6);
	CHECK_EQ(rodata[0], u8{ 'H' });
	CHECK_EQ(rodata[4], u8{ 'o' });
	CHECK_EQ(rodata[5], u8{ 0 });
}

// --- Local label scoping --------------------------------------------------------------------

TEST(encoding, local_labels_are_scoped_to_their_parent)
{
	// The same local name under two parents must resolve to two different targets.
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		".loop:\r\n"
		"    jp .loop\r\n"
		"second:\r\n"
		"    nop\r\n"
		".loop:\r\n"
		"    jp .loop\r\n");

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	auto words = r.words();
	CHECK_EQ(words.size(), 3u);
	CHECK_EQ(Instruction(words[0]).simm24().signedValue(), 0);
	CHECK_EQ(Instruction(words[2]).simm24().signedValue(), 0);
}

TEST(encoding, ifeq_expands_to_a_compare_and_a_jump)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    ifeq r1, r2, .same\r\n"
		"    nop\r\n"
		".same:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	Instruction compare{ r.words()[0] };
	Instruction jump{ r.words()[1] };
	CHECK_EQ(compare.opcode() == Opcode::CMP, true);
	CHECK_EQ(compare.rs(), u8{ 1 });
	CHECK_EQ(compare.rt(), u8{ 2 });
	CHECK_EQ(jump.opcode() == Opcode::JZ, true);
	// The displacement is measured from the jump itself, not from the statement: past the nop.
	CHECK_EQ(jump.simm24().signedValue(), 8);
}

TEST(encoding, an_ordering_if_uses_its_own_opcode)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    ifls r1, 10, .less\r\n"
		".less:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(Instruction(r.words()[0]).opcode() == Opcode::CMPI, true);
	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 10 });
	CHECK_EQ(Instruction(r.words()[1]).opcode() == Opcode::JLS, true);
}

TEST(encoding, comparing_floats_picks_fcmp)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    ifgr f1, f2, .bigger\r\n"
		".bigger:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(Instruction(r.words()[0]).opcode() == Opcode::FCMP, true);
	CHECK_EQ(Instruction(r.words()[1]).opcode() == Opcode::JGR, true);
}

TEST(encoding, a_conditional_jump_alias_picks_the_register_form_for_a_register)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    jeq r4\r\n"
		"    jls r5\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(Instruction(r.words()[0]).opcode() == Opcode::JZR, true);
	CHECK_EQ(Instruction(r.words()[1]).opcode() == Opcode::JLSR, true);
	CHECK_EQ(Instruction(r.words()[1]).rs(), u8{ 5 });
}

// LI only reaches 16 bits and LA only takes symbols, so a 32-bit constant had to be written as a
// LUI/ORI pair by hand.
TEST(encoding, lc_loads_a_full_32_bit_constant)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    lc r1, 0x12345678\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	Instruction upper{ r.words()[0] };
	Instruction lower{ r.words()[1] };
	CHECK_EQ(upper.opcode() == Opcode::LUI, true);
	CHECK_EQ(upper.imm16(), u16{ 0x1234 });
	CHECK_EQ(lower.opcode() == Opcode::ORI, true);
	CHECK_EQ(lower.imm16(), u16{ 0x5678 });
	CHECK_EQ(lower.rd(), u8{ 1 });
}

TEST(encoding, swap_exchanges_without_a_temporary)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    swap r3, r4\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	// Three XORs, and never a register the programmer did not name.
	for (usize i = 0; i < 3; ++i)
		CHECK_EQ(Instruction(r.words()[i]).opcode() == Opcode::XOR, true);
	CHECK_EQ(Instruction(r.words()[0]).rd(), u8{ 3 });
	CHECK_EQ(Instruction(r.words()[1]).rd(), u8{ 4 });
	CHECK_EQ(Instruction(r.words()[2]).rd(), u8{ 3 });
}

TEST(encoding, enter_and_leave_are_the_only_things_that_touch_fp)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    enter\r\n"
		"    leave\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(Instruction(r.words()[0]).opcode() == Opcode::PUSH, true);
	CHECK_EQ(Instruction(r.words()[0]).rs(), u8{ 14 });
	CHECK_EQ(Instruction(r.words()[1]).opcode() == Opcode::MOV, true);
	CHECK_EQ(Instruction(r.words()[1]).rd(), u8{ 14 });
	CHECK_EQ(Instruction(r.words()[1]).rs(), u8{ 15 });
	CHECK_EQ(Instruction(r.words()[2]).opcode() == Opcode::MOV, true);
	CHECK_EQ(Instruction(r.words()[2]).rd(), u8{ 15 });
	CHECK_EQ(Instruction(r.words()[3]).opcode() == Opcode::POP, true);
	CHECK_EQ(Instruction(r.words()[3]).rd(), u8{ 14 });
}

TEST(encoding, the_small_pseudo_instructions_expand_as_documented)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    inc r1\r\n"
		"    dec r2\r\n"
		"    clr r3\r\n"
		"    tst r4\r\n"
		"    jmp .done\r\n"
		".done:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(Instruction(r.words()[0]).opcode() == Opcode::ADDI, true);
	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 1 });
	CHECK_EQ(Instruction(r.words()[1]).opcode() == Opcode::SUBI, true);
	CHECK_EQ(Instruction(r.words()[2]).opcode() == Opcode::LI, true);
	CHECK_EQ(Instruction(r.words()[2]).imm16(), u16{ 0 });
	CHECK_EQ(Instruction(r.words()[3]).opcode() == Opcode::CMPI, true);
	CHECK_EQ(Instruction(r.words()[4]).opcode() == Opcode::JP, true);
}

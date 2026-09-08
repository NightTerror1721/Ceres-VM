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
	// register index under the old encoding. 0x70F0 rather than 0xF0F0 because the field is signed
	// now - this is a decoding test, and a negative displacement would make it a sign test.
	AssembleResult r;
	auto words = assembleText("    str r2, [r1 + 28912]", r);

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const Instruction encoded{ words[0] };
	CHECK_EQ(encoded.simm16(), i16{ 28912 });   // 0x70F0
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
	// 40 KiB of .data in front of it puts `counter` past the reach of a PC-relative store, so
	// this is the long form: the one that has to materialise the address somewhere.
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    let padding: u8[40960] = [1]\r\n"
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

	// And it must be `at` (R13), the assembler temporary. This was R12 while R13 was still
	// called the Link Register - a name for something the machine never did, since CALL pushes
	// the return address on the stack and nothing reads R13.
	CHECK_EQ(lower.rd(), u8{ 13 });

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

	// One instruction each, and a bare `enter` opens a frame of nothing.
	CHECK_EQ(r.words().size(), 3u);
	CHECK_EQ(Instruction(r.words()[0]).opcode() == Opcode::ENTER, true);
	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 0 });
	CHECK_EQ(Instruction(r.words()[1]).opcode() == Opcode::LEAVE, true);
	CHECK_EQ(Instruction(r.words()[2]).opcode() == Opcode::RET, true);
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

// `[r1 - 8]` used to assemble to a displacement of 65528 and reach the wrong memory. The field is
// signed now, so it encodes -8 and the disassembler renders it as one.
TEST(encoding, a_negative_displacement_encodes_as_a_negative_field)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    ldr r1, [r2 - 8]\r\n"
		"    str r3, [r4 - 12]\r\n"
		"    lea r5, [r6 - 16]\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(Instruction(r.words()[0]).simm16(), i16{ -8 });
	CHECK_EQ(Instruction(r.words()[1]).simm16(), i16{ -12 });
	CHECK_EQ(Instruction(r.words()[2]).simm16(), i16{ -16 });

	CHECK(r.listing().find("[r2 - 8]") != std::string::npos);
}

TEST(encoding, a_displacement_outside_the_signed_range_is_rejected)
{
	// It used to truncate silently: 70000 became 4464.
	AssembleResult tooBig = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    ldr r1, [r2 + 70000]\r\n"
		"    ret\r\n");

	CHECK(!tooBig.ok());
	CHECK(tooBig.joinedErrors().find("signed 16-bit") != std::string::npos);

	// 32767 is the largest that fits, and still assembles.
	AssembleResult atTheEdge = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    ldr r1, [r2 + 32767]\r\n"
		"    ret\r\n");

	CHECK(atTheEdge.ok());
	if (!atTheEdge.ok()) Registry::instance().recordFailure(atTheEdge.joinedErrors());
}

TEST(encoding, an_index_register_picks_the_indexed_opcode)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    ldr  r1, [r2 + r3]\r\n"
		"    ldr  r1, [r2 + 4]\r\n"
		"    str  r1, [r2 + r3]\r\n"
		"    ldrb r1, [r2 + r3]\r\n");
	auto words = r.words();

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(words.size(), 4u);

	const Instruction indexedLoad{ words[0] };
	CHECK_EQ(indexedLoad.opcode() == Opcode::LDRX, true);
	CHECK_EQ(indexedLoad.rd(), u8{ 1 });
	CHECK_EQ(indexedLoad.rs(), u8{ 2 });
	CHECK_EQ(indexedLoad.rt(), u8{ 3 });

	// A displacement still picks the displacement form: the mnemonic is the same either way.
	CHECK_EQ(Instruction{ words[1] }.opcode() == Opcode::LDR, true);

	// The store keeps the base in Rd and the value in Rs, because Rt is the index now.
	const Instruction indexedStore{ words[2] };
	CHECK_EQ(indexedStore.opcode() == Opcode::STRX, true);
	CHECK_EQ(indexedStore.rd(), u8{ 2 });
	CHECK_EQ(indexedStore.rs(), u8{ 1 });
	CHECK_EQ(indexedStore.rt(), u8{ 3 });

	CHECK_EQ(Instruction{ words[3] }.opcode() == Opcode::LDRBX, true);
}

TEST(encoding, a_register_index_cannot_be_subtracted)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    ldr r1, [r2 - r3]\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("cannot be subtracted") != std::string::npos);
}

TEST(encoding, a_register_alias_can_be_an_index)
{
	AssembleResult r = assembleSource(
		"alias cursor = r5\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ldrb r1, [r2 + cursor]\r\n");
	auto words = r.words();

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(Instruction{ words[0] }.opcode() == Opcode::LDRBX, true);
	CHECK_EQ(Instruction{ words[0] }.rt(), u8{ 5 });
}

TEST(encoding, ldvp_reaches_a_variable_in_one_word)
{
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    let counter: u32 = 7\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ldvp r1, counter\r\n"
		"    stvp r1, counter\r\n");
	auto words = r.words();

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	// One word each, where ldv/stv take three.
	CHECK_EQ(words.size(), 2u);

	const Instruction load{ words[0] };
	const Instruction store{ words[1] };
	CHECK_EQ(load.opcode() == Opcode::LDRP, true);
	CHECK_EQ(store.opcode() == Opcode::STRP, true);
	CHECK_EQ(load.rd(), u8{ 1 });
	CHECK_EQ(store.rs(), u8{ 1 });

	// The displacement is measured from the instruction itself: .data starts right after the two
	// words of .text, so the first one has to reach exactly that far.
	const u32 textSize = 2 * Instruction::Size;
	CHECK_EQ(load.simm16(), static_cast<i16>(textSize));
	CHECK_EQ(store.simm16(), static_cast<i16>(textSize - Instruction::Size));

	// And nothing was borrowed to hold an address.
	CHECK(load.rd() != 13);
	CHECK(store.rs() != 13);
}

TEST(encoding, the_scalar_type_picks_the_pc_relative_width)
{
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    let small: i8 = -1\r\n"
		"    let wide: u16 = 2\r\n"
		"    let real: f32 = 1.5\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ldvp r1, small\r\n"
		"    ldvp r2, wide\r\n"
		"    ldvp f0, real\r\n");
	auto words = r.words();

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(Instruction{ words[0] }.opcode() == Opcode::LDRSBP, true);
	CHECK_EQ(Instruction{ words[1] }.opcode() == Opcode::LDRHP, true);
	CHECK_EQ(Instruction{ words[2] }.opcode() == Opcode::FLDRP, true);
}

TEST(encoding, a_variable_out_of_pc_relative_reach_is_reported)
{
	// 40 KiB of .bss between the instruction and the variable puts it past a signed 16-bit
	// displacement, which is an error rather than a wrap to somewhere arbitrary.
	AssembleResult r = assembleSource(
		"@bss\r\n"
		"    let padding: u8[40960]\r\n"
		"    let far: u32\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ldvp r1, far\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("out of reach") != std::string::npos);
}

TEST(encoding, enter_takes_the_frame_size_as_an_immediate)
{
	AssembleResult r = assembleSource(
		"struct Frame\r\n"
		"    saved_r8: u32\r\n"
		"    count: u32\r\n"
		"endstruct\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    enter Frame\r\n"
		"    leave\r\n");
	auto words = r.words();

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	// A prologue that used to be three instructions and twelve bytes.
	CHECK_EQ(words.size(), 2u);
	CHECK_EQ(Instruction{ words[0] }.opcode() == Opcode::ENTER, true);
	CHECK_EQ(Instruction{ words[0] }.imm16(), u16{ 8 }); // the struct's own size
}

TEST(encoding, bl_picks_its_form_from_the_second_operand)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    bl r11, helper\r\n"
		"    bl r11, r5\r\n"
		"helper:\r\n"
		"    jp r11\r\n");
	auto words = r.words();

	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	const Instruction linked{ words[0] };
	CHECK_EQ(linked.opcode() == Opcode::BL, true);
	CHECK_EQ(linked.rd(), u8{ 11 });
	CHECK_EQ(linked.simm20(), 8); // two instructions forward, measured from the bl itself

	const Instruction indirect{ words[1] };
	CHECK_EQ(indirect.opcode() == Opcode::BLR, true);
	CHECK_EQ(indirect.rd(), u8{ 11 });
	CHECK_EQ(indirect.rs(), u8{ 5 });
}

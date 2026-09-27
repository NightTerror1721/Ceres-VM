// The 64-bit instructions' decoding (plan/v2 SPEC 6): which words the machine runs, why it refuses the others, and
// how the disassembler writes them.
#include "framework.h"
#include <ceres/core/isa/wide.h>
#include <ceres/core/isa/disassembler.h>
#include <string>

using namespace ceres;
using namespace ceres::isa;

// The opcodes SPEC 6.4 assigns, and nothing else.
static_assert(static_cast<u8>(Opcode::ADD64) == 0xD7 && static_cast<u8>(Opcode::LDRDP) == 0xED);
static_assert(static_cast<u8>(Opcode::FADDD) == 0xEF && static_cast<u8>(Opcode::FSTRD) == 0xFE);
static_assert(static_cast<u8>(Opcode::FLDRDX) == 0x7A && static_cast<u8>(Opcode::MFFD) == 0x7E);
static_assert(!wide::isWide(static_cast<Opcode>(0xEE)) && !wide::isWide(static_cast<Opcode>(0xFF)) && !wide::isWide(static_cast<Opcode>(0x7F)));
static_assert([] {
	u32 count = 0;
	for (u32 opcode = 0; opcode < 256; ++opcode)
		count += wide::isWide(static_cast<Opcode>(opcode)) ? 1u : 0u;
	return count == 44;
}());

// The subfields sit where SPEC 6.4 puts them.
static_assert(fields::ShiftKind::Mask == 0xC0u && fields::ShiftAmount::Mask == 0x3Fu);
static_assert(fields::BitOp::Mask == 0x3u && fields::MinMax::Mask == 0x1u && fields::UnaryOp::Mask == 0x7u && fields::FcvtKind::Mask == 0xFu);

// A pair is stored as its even register's number.
static_assert(Instruction::ADD64(1, 2, 3).rd() == 2 && Instruction::ADD64(1, 2, 3).rs() == 4 && Instruction::ADD64(1, 2, 3).rt() == 6);
static_assert(Instruction::FCVT(static_cast<u8>(wide::FcvtKind::DL), 4, 2).rd() == 8 && Instruction::FCVT(static_cast<u8>(wide::FcvtKind::DL), 4, 2).rs() == 4);
static_assert(Instruction::FCVT(static_cast<u8>(wide::FcvtKind::WD), 5, 3).rd() == 5 && Instruction::FCVT(static_cast<u8>(wide::FcvtKind::WD), 5, 3).rs() == 6);

namespace
{
	Instruction raw(Opcode opcode, u8 rd, u8 rs, u8 rt, u8 low = 0)
	{
		return Instruction::make(opcode, rd, rs, rt, low);
	}
}

TEST(wide_decoding, every_factory_builds_a_word_the_machine_runs)
{
	const Instruction words[] = {
		Instruction::ADD64(0, 1, 2), Instruction::SUB64(6, 5, 4), Instruction::NEG64(3, 3), Instruction::CMP64(1, 2),
		Instruction::MULL(1, 7, 9), Instruction::IMULL(2, 15, 14), Instruction::MUL64(1, 2, 3), Instruction::DIV64(1, 2, 3),
		Instruction::IDIV64(1, 2, 3), Instruction::MOD64(1, 2, 3), Instruction::IMOD64(1, 2, 3), Instruction::SHL64(1, 2, 15),
		Instruction::SHR64(1, 2, 3), Instruction::SAR64(1, 2, 3), Instruction::SHL64I(1, 2, 63), Instruction::SHR64I(1, 2, 0),
		Instruction::SAR64I(1, 2, 32), Instruction::SXT64(1, 13), Instruction::CLZ64(13, 6), Instruction::CTZ64(0, 0),
		Instruction::POPCNT64(15, 1), Instruction::MOV64(6, 0), Instruction::LDRD(1, 15, 0xFFF8), Instruction::STRD(14, 2, 8),
		Instruction::LDRDX(1, 2, 3), Instruction::STRDX(2, 1, 3), Instruction::LDRDP(1, -16), Instruction::FADDD(7, 6, 5),
		Instruction::FSUBD(0, 1, 2), Instruction::FMULD(0, 1, 2), Instruction::FDIVD(0, 1, 2), Instruction::FMAD(0, 1, 2),
		Instruction::FSQRTD(7, 0), Instruction::FCMPD(1, 2), Instruction::FMIND(0, 1, 2), Instruction::FMAXD(0, 1, 2),
		Instruction::FMODD(0, 1, 2), Instruction::FNEGD(1, 2), Instruction::FTRUNCD(1, 2), Instruction::FCOPYSIGND(0, 1, 2),
		Instruction::FCLASSD(3, 7), Instruction::FMOVD(1, 2), Instruction::FLDRD(1, 2, 4), Instruction::FSTRD(2, 1, 4),
		Instruction::FLDRDX(1, 2, 3), Instruction::FSTRDX(2, 1, 3), Instruction::FLDRDP(7, 64), Instruction::MTFD(7, 6),
		Instruction::MFFD(6, 7),
	};
	for (const Instruction word : words)
		CHECK(wide::check(word) == wide::Decode::Ok);

	for (u8 kind = 0; kind < wide::FcvtKindCount; ++kind)
		CHECK(wide::check(Instruction::FCVT(kind, 1, 2)) == wide::Decode::Ok);
}

TEST(wide_decoding, an_odd_pair_or_x7_is_a_register_pair_fault)
{
	CHECK(wide::check(raw(Opcode::ADD64, 1, 2, 4)) == wide::Decode::RegisterPair);   // rd odd
	CHECK(wide::check(raw(Opcode::ADD64, 2, 3, 4)) == wide::Decode::RegisterPair);   // rs odd
	CHECK(wide::check(raw(Opcode::ADD64, 2, 4, 5)) == wide::Decode::RegisterPair);   // rt odd
	CHECK(wide::check(raw(Opcode::ADD64, 14, 2, 4)) == wide::Decode::RegisterPair);  // x7 is fp:sp
	CHECK(wide::check(raw(Opcode::CMP64, 0, 14, 2)) == wide::Decode::RegisterPair);
	CHECK(wide::check(raw(Opcode::FADDD, 14, 12, 0)) == wide::Decode::Ok);           // d7 is fine
	CHECK(wide::check(raw(Opcode::FADDD, 15, 12, 0)) == wide::Decode::RegisterPair);
	// A field that is a single register may be odd, and may be r14 or r15.
	CHECK(wide::check(raw(Opcode::MULL, 2, 15, 13)) == wide::Decode::Ok);
	CHECK(wide::check(raw(Opcode::LDRD, 2, 15, 0, 8)) == wide::Decode::Ok);
	CHECK(wide::check(raw(Opcode::STRD, 15, 2, 0, 8)) == wide::Decode::Ok);
	CHECK(wide::check(raw(Opcode::STRD, 15, 3, 0, 8)) == wide::Decode::RegisterPair);
	// An unused field is not looked at.
	CHECK(wide::check(raw(Opcode::NEG64, 2, 4, 7)) == wide::Decode::Ok);
	// fcvt's fields are pairs or not by its kind: fcvt.w.d takes any rd, and an even ds.
	CHECK(wide::check(Instruction::make(Opcode::FCVT, 5, 6, 0, static_cast<u8>(wide::FcvtKind::WD))) == wide::Decode::Ok);
	CHECK(wide::check(Instruction::make(Opcode::FCVT, 5, 7, 0, static_cast<u8>(wide::FcvtKind::WD))) == wide::Decode::RegisterPair);
	CHECK(wide::check(Instruction::make(Opcode::FCVT, 14, 6, 0, static_cast<u8>(wide::FcvtKind::LD))) == wide::Decode::RegisterPair);
}

TEST(wide_decoding, a_subfield_no_instruction_has_is_a_bad_subfield)
{
	CHECK(wide::check(raw(Opcode::SHI64, 2, 4, 0, 0xC0)) == wide::Decode::BadSubfield);   // shift kind 3
	CHECK(wide::check(raw(Opcode::BITS64, 1, 2, 0, 3)) == wide::Decode::BadSubfield);     // bit op 3
	CHECK(wide::check(raw(Opcode::FUNARYD, 2, 4, 0, 6)) == wide::Decode::BadSubfield);
	CHECK(wide::check(raw(Opcode::FUNARYD, 2, 4, 0, 7)) == wide::Decode::BadSubfield);
	CHECK(wide::check(raw(Opcode::FCVT, 2, 4, 0, 14)) == wide::Decode::BadSubfield);
	CHECK(wide::check(raw(Opcode::FCVT, 2, 4, 0, 15)) == wide::Decode::BadSubfield);
	// Checked before the pairs: an invalid subfield says nothing about which fields are pairs.
	CHECK(wide::check(raw(Opcode::FCVT, 1, 3, 0, 15)) == wide::Decode::BadSubfield);
	// Every value of a one-bit subfield is an instruction.
	CHECK(wide::check(raw(Opcode::FMINMAXD, 2, 4, 6, 1)) == wide::Decode::Ok);
}

TEST(wide_decoding, the_disassembler_writes_them_the_way_spec_does)
{
	CHECK_EQ(Disassembler::disassemble(Instruction::ADD64(1, 2, 3)), std::string("ADD64 x1, x2, x3"));
	CHECK_EQ(Disassembler::disassemble(Instruction::CMP64(0, 6)), std::string("CMP64 x0, x6"));
	CHECK_EQ(Disassembler::disassemble(Instruction::MULL(1, 7, 9)), std::string("MULL x1, r7, r9"));
	CHECK_EQ(Disassembler::disassemble(Instruction::SAR64I(2, 3, 40)), std::string("SAR64 x2, x3, 40"));
	CHECK_EQ(Disassembler::disassemble(Instruction::SHL64(2, 3, 5)), std::string("SHL64 x2, x3, r5"));
	CHECK_EQ(Disassembler::disassemble(Instruction::POPCNT64(4, 1)), std::string("POPCNT64 r4, x1"));
	CHECK_EQ(Disassembler::disassemble(Instruction::LDRD(1, 14, static_cast<u16>(-8))), std::string("LDRD x1, [r14 - 8]"));
	CHECK_EQ(Disassembler::disassemble(Instruction::STRD(15, 3, 16)), std::string("STRD [r15 + 16], x3"));
	CHECK_EQ(Disassembler::disassemble(Instruction::STRDX(2, 1, 3)), std::string("STRD [r2 + r3], x1"));
	CHECK_EQ(Disassembler::disassemble(Instruction::LDRDP(0, 32)), std::string("LDRD x0, [pc + 32]"));
	CHECK_EQ(Disassembler::disassemble(Instruction::FCVT(static_cast<u8>(wide::FcvtKind::DL), 4, 2)), std::string("FCVT.D.L d4, x2"));
	CHECK_EQ(Disassembler::disassemble(Instruction::FCVT(static_cast<u8>(wide::FcvtKind::SD), 3, 1)), std::string("FCVT.S.D f3, d1"));
	CHECK_EQ(Disassembler::disassemble(Instruction::FMAXD(0, 1, 7)), std::string("FMAX.D d0, d1, d7"));
	CHECK_EQ(Disassembler::disassemble(Instruction::FCEILD(5, 6)), std::string("FCEIL.D d5, d6"));
	CHECK_EQ(Disassembler::disassemble(Instruction::FCLASSD(9, 2)), std::string("FCLASS.D r9, d2"));
	CHECK_EQ(Disassembler::disassemble(Instruction::FSTRDX(1, 2, 3)), std::string("FSTR.D [r1 + r3], d2"));
	CHECK_EQ(Disassembler::disassemble(Instruction::MTFD(3, 2)), std::string("MTF.D d3, x2"));

	// A word the machine refuses says so rather than reading as something plausible.
	CHECK_EQ(Disassembler::disassemble(raw(Opcode::ADD64, 1, 2, 4)), std::string("<ADD64 with an invalid register pair>"));
	CHECK_EQ(Disassembler::disassemble(raw(Opcode::FCVT, 2, 4, 0, 15)), std::string("<opcode 0xfb with an invalid subfield>"));
	CHECK_EQ(Disassembler::disassemble(Instruction(0xEE000000u)), std::string("<unknown opcode 0xee>"));
}

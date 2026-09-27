// The 64-bit instructions in the assembler (plan/v2 SPEC 6): register pairs, the mnemonics, 64-bit data and li64. Every
// 64-bit opcode assembles from the text the disassembler writes for it and back.
#include "framework.h"
#include "assemble_helper.h"
#include <ceres/core/isa/wide.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <set>

using namespace ceres;
using namespace ceres::fmt;
using namespace ceres::isa;
using namespace ceres::testing;

namespace
{
	std::string lower(std::string_view text)
	{
		std::string out(text);
		for (char& c : out)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return out;
	}

	u64 read64(std::span<const u8> bytes, usize at)
	{
		u64 value = 0;
		for (usize i = 0; i < 8; ++i)
			value |= static_cast<u64>(bytes[at + i]) << (8 * i);
		return value;
	}

	// One source line per form, written exactly as the disassembler writes it back.
	constexpr std::array<std::string_view, 67> RoundTrip{
		"add64 x1, x2, x3", "sub64 x0, x6, x5", "neg64 x3, x3", "cmp64 x1, x2", "mull x1, r7, r9", "imull x2, r15, r14",
		"mul64 x1, x2, x3", "div64 x4, x5, x6", "idiv64 x1, x2, x3", "mod64 x1, x2, x3", "imod64 x1, x2, x3",
		"shl64 x1, x2, r15", "shr64 x1, x2, r3", "sar64 x1, x2, r3", "shl64 x1, x2, 63", "shr64 x1, x2, 0", "sar64 x6, x0, 32",
		"sxt64 x1, r13", "clz64 r13, x6", "ctz64 r0, x0", "popcnt64 r15, x1", "mov64 x6, x0",
		"ldrd x1, [r15 + 8]", "ldrd x2, [r14 - 8]", "strd [r14 + 16], x2", "ldrd x1, [r2 + r3]", "strd [r2 + r3], x1",
		"fadd.d d7, d6, d5", "fsub.d d0, d1, d2", "fmul.d d0, d1, d2", "fdiv.d d0, d1, d2", "fma.d d0, d1, d2",
		"fsqrt.d d7, d0", "fcmp.d d1, d2", "fmin.d d0, d1, d2", "fmax.d d0, d1, d2", "fmod.d d0, d1, d2",
		"fneg.d d1, d2", "fabs.d d1, d2", "fround.d d1, d2", "ffloor.d d1, d2", "fceil.d d1, d2", "ftrunc.d d1, d2",
		"fcopysign.d d0, d1, d2", "fclass.d r3, d7", "fmov.d d1, d2", "fldr.d d1, [r2 + 4]", "fstr.d [r2 - 4], d1",
		"fldr.d d1, [r2 + r3]", "fstr.d [r2 + r3], d1", "mtf.d d7, x6", "mff.d x6, d7",
		"fcvt.d.s d1, f3", "fcvt.s.d f3, d1", "fcvt.d.w d1, r3", "fcvt.d.wu d1, r3", "fcvt.w.d r3, d1", "fcvt.wu.d r3, d1",
		"fcvt.d.l d1, x3", "fcvt.d.lu d1, x3", "fcvt.l.d x3, d1", "fcvt.lu.d x3, d1", "fcvt.s.l f3, x1", "fcvt.s.lu f3, x1",
		"fcvt.l.s x1, f3", "fcvt.lu.s x1, f3", "mov64 x0, x0",
	};
}

TEST(wide_assembly, every_form_assembles_to_what_the_disassembler_reads_back)
{
	std::set<u8> opcodes;
	for (const std::string_view line : RoundTrip)
	{
		const AssembleResult r = assembleSource(inText(std::format("    {}\r\n    halt", line)));
		CHECK(r.ok());
		if (!r.ok())
		{
			::ceres::testing::Registry::instance().recordFailure(std::format("{}: {}", line, r.joinedErrors()));
			continue;
		}
		const auto words = r.words();
		CHECK_EQ(words.size(), usize{ 2 });
		const Instruction word(words[0]);
		CHECK(wide::check(word) == wide::Decode::Ok);
		CHECK_EQ(lower(Disassembler::disassemble(word)), std::string(line));
		opcodes.insert(static_cast<u8>(word.opcode()));
	}

	// The two PC-relative loads come from a variable within reach (li64 and ldv below); every other 64-bit opcode is here.
	opcodes.insert(static_cast<u8>(Opcode::LDRDP));
	opcodes.insert(static_cast<u8>(Opcode::FLDRDP));
	for (u32 opcode = 0; opcode < 256; ++opcode)
	{
		if (wide::isWide(static_cast<Opcode>(opcode)) && !opcodes.contains(static_cast<u8>(opcode)))
			::ceres::testing::Registry::instance().recordFailure(std::format("opcode {:#04x} has no round trip", opcode));
	}
}

TEST(wide_assembly, pairs_are_case_blind_and_take_an_alias)
{
	const AssembleResult r = assembleSource(inText("    alias acc = x2\r\n    ADD64 acc, acc, X1\r\n    FADD.D D3, d3, D0\r\n    halt"));
	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }
	const auto words = r.words();
	CHECK_EQ_FMT(words[0], static_cast<u32>(Instruction::ADD64(2, 2, 1)), renderWord);
	CHECK_EQ_FMT(words[1], static_cast<u32>(Instruction::FADDD(3, 3, 0)), renderWord);
}

TEST(wide_assembly, sixty_four_bit_data_is_eight_bytes_low_word_first)
{
	const AssembleResult r = assembleSource(
		"@rodata\r\n"
		"global let a: u64 = 0x123456789ABCDEF0\r\n"
		"global let b: i64 = -5\r\n"
		"global let c: f64 = 3.141592653589793\r\n"
		"global let d: i64 = 0x80000000 * 4\r\n"
		"global let e: u64[2] = [1, 0xFFFFFFFFFFFFFFFF]\r\n"
		"const BIG: u64 = 1 + 0x100000000\r\n"
		"global let g: u64 = BIG * 2\r\n"
		"global let h: f64 = 1.0 / 3.0\r\n"
		"global let size: u32 = sizeof(e)\r\n"
		"@text\r\nglobal main:\r\n    halt\r\n");
	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto rodata = r.program->rodata();
	CHECK_EQ(rodata.size(), usize{ 8 * 8 + 4 });
	CHECK_EQ(read64(rodata, 0), u64{ 0x123456789ABCDEF0 });
	CHECK_EQ(read64(rodata, 8), static_cast<u64>(i64{ -5 }));
	CHECK_EQ(read64(rodata, 16), std::bit_cast<u64>(3.141592653589793));
	CHECK_EQ(read64(rodata, 24), u64{ 0x200000000 });
	CHECK_EQ(read64(rodata, 32), u64{ 1 });
	CHECK_EQ(read64(rodata, 40), ~u64{ 0 });
	CHECK_EQ(read64(rodata, 48), u64{ 0x200000002 });
	CHECK_EQ(read64(rodata, 56), std::bit_cast<u64>(1.0 / 3.0));   // worked out in double precision
	CHECK_EQ(rodata[64], u8{ 16 });                                 // sizeof(e)
}

TEST(wide_assembly, a_float_outside_a_64_bit_context_is_the_float_it_always_was)
{
	const AssembleResult r = assembleSource(
		"@rodata\r\nglobal let f: f32 = 0.1\r\nglobal let g: f32 = 1.0 / 3.0\r\n@text\r\nglobal main:\r\n    halt\r\n");
	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }
	const auto rodata = r.program->rodata();
	u32 f = 0, g = 0;
	std::memcpy(&f, rodata.data(), 4);
	std::memcpy(&g, rodata.data() + 4, 4);
	CHECK_EQ(f, std::bit_cast<u32>(0.1f));
	CHECK_EQ(g, std::bit_cast<u32>(1.0f / 3.0f));
}

TEST(wide_assembly, li64_loads_a_pooled_constant_in_one_word_when_it_reaches)
{
	const AssembleResult r = assembleSource(
		"@data\r\nglobal let total: i64 = 7\r\nglobal let ratio: f64 = 0.5\r\n"
		"@text\r\nglobal main:\r\n"
		"    li64 x1, 0x123456789ABCDEF0\r\n"
		"    li64 x2, -1\r\n"
		"    ldv x3, total\r\n"
		"    ldrd x4, [total]\r\n"
		"    ldv d1, 2.5\r\n"
		"    fldr.d d2, [ratio]\r\n"
		"    stv total, x3\r\n"
		"    halt\r\n");
	CHECK(r.ok());
	if (!r.ok()) { ::ceres::testing::Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto words = r.words();
	CHECK_EQ(words.size(), usize{ 6 + 3 + 1 });   // six loads relaxed to one word each; the store is three
	for (usize i = 0; i < 4; ++i)
		CHECK(Instruction(words[i]).opcode() == Opcode::LDRDP);
	CHECK(Instruction(words[4]).opcode() == Opcode::FLDRDP);
	CHECK(Instruction(words[5]).opcode() == Opcode::FLDRDP);
	CHECK(Instruction(words[8]).opcode() == Opcode::STRD);
	CHECK_EQ(Instruction(words[0]).rd(), u8{ 2 });   // x1

	// The pool is the constants, in .rodata, eight bytes each.
	const auto rodata = r.program->rodata();
	CHECK_EQ(rodata.size(), usize{ 24 });
	CHECK_EQ(read64(rodata, 0), u64{ 0x123456789ABCDEF0 });
	CHECK_EQ(read64(rodata, 8), ~u64{ 0 });
	CHECK_EQ(read64(rodata, 16), std::bit_cast<u64>(2.5));
}

TEST(wide_assembly, a_register_of_the_wrong_bank_says_which_one_goes_there)
{
	const struct { std::string_view source; std::string_view expected; } cases[] = {
		{ "add64 x1, r2, x3", "takes an integer pair (x0-x6) as operand 2, not an integer register (r0-r15)" },
		{ "mull r2, r4, r5", "takes an integer pair (x0-x6) as operand 1" },
		{ "fadd.d d1, f2, d3", "takes a double pair (d0-d7) as operand 2, not a float register (f0-f15)" },
		{ "fcvt.w.d r1, x2", "takes a double pair (d0-d7) as operand 2, not an integer pair (x0-x6)" },
		{ "add64 x7, x1, x2", "'x7' is not a register pair" },
		{ "ldrd x1, [x2 + 4]", "must be a general-purpose register" },
		{ "shl64 x1, x2, 64", "a 64-bit shift takes 0 to 63" },
		{ "li r1, 0x100000000", "4294967296 does not fit in 32 bits" },
	};
	for (const auto& c : cases)
	{
		const AssembleResult r = assembleSource(inText(std::format("    {}\r\n    halt", c.source)));
		CHECK(!r.ok());
		if (r.joinedErrors().find(c.expected) == std::string::npos)
			::ceres::testing::Registry::instance().recordFailure(std::format("{}: {}", c.source, r.joinedErrors()));
	}
}

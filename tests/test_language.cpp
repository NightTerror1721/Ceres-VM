// Phase 4 language features: constant expressions, data alignment and the instructions that
// were defined in the enum but had no way to be written.

#include "framework.h"
#include "assemble_helper.h"
#include "vm/ceresvm.h"
#include "vm/bios.h"
#include "vm/memory.h"

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
		explicit Machine(std::initializer_list<Instruction> program)
		{
			const Address entry = Memory::UnrestrictedSegmentStart;

			usize offset = 0;
			for (Instruction instruction : program)
			{
				_vm.memory().writeUnchecked<u32>(entry + Address(static_cast<u32>(offset)), instruction.raw());
				offset += Instruction::Size;
			}

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
	};
}

// --- Constant expressions --------------------------------------------------------------------

TEST(language, an_immediate_can_be_an_arithmetic_expression)
{
	AssembleResult r = assembleSource(inText("    li r1, 2 + 3 * 4"));

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	// Precedence, not left to right: 14, never 20.
	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 14 });
}

TEST(language, subtraction_and_division_fold_too)
{
	AssembleResult r = assembleSource(inText("    li r1, 100 / 4 - 5"));

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 20 });
}

TEST(language, a_leading_minus_negates_the_expression)
{
	AssembleResult r = assembleSource(inText("    li r1, 0 - 2 * 3"));

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 0xFFFA });   // -6
}

TEST(language, a_constant_declaration_folds_its_initialiser)
{
	AssembleResult r = assembleSource(
		"const BLOCK = 16 * 4\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, BLOCK\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 64 });
}

TEST(language, division_by_zero_in_a_constant_expression_is_reported)
{
	AssembleResult r = assembleSource(inText("    li r1, 8 / 0"));

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("Division by zero") != std::string::npos);
}

TEST(language, an_identifier_inside_a_larger_expression_is_reported_clearly)
{
	// Constants are not resolved until the unit is built, so the parser cannot fold them. The
	// diagnostic has to say that rather than fail obscurely.
	AssembleResult r = assembleSource(
		"const BASE = 4\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 2 * BASE\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("cannot reference") != std::string::npos);
}

TEST(language, an_identifier_on_its_own_still_works)
{
	AssembleResult r = assembleSource(
		"const BASE = 41\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, BASE\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 41 });
}

// --- Alignment ---------------------------------------------------------------------------------

TEST(language, a_word_variable_is_aligned_after_an_odd_sized_one)
{
	AssembleResult r = assembleSource(
		"@rodata\r\n"
		"    let tag: u8[3] = \"ab\"\r\n"
		"    let value: u32 = 1\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	// 3 bytes of string, one byte of padding, then the word: eight in total, not seven.
	CHECK_EQ(r.program->rodata().size(), usize{ 8 });

	const auto rodata = r.program->rodata();
	CHECK_EQ(rodata[3], u8{ 0 });   // the padding byte
	CHECK_EQ(rodata[4], u8{ 1 });   // the word starts on a multiple of four
}

TEST(language, a_halfword_variable_gets_two_byte_alignment)
{
	AssembleResult r = assembleSource(
		"@rodata\r\n"
		"    let flag: u8 = 7\r\n"
		"    let pair: u16 = 258\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(r.program->rodata().size(), usize{ 4 });
	CHECK_EQ(r.program->rodata()[2], u8{ 2 });   // 258 = 0x0102, little-endian
}

TEST(language, the_layout_and_the_emitted_bytes_agree_after_padding)
{
	// The symbol's address comes from the translation unit and the bytes from the emitter. If
	// the two disagreed about padding, a load through the symbol would read the wrong place.
	AssembleResult r = assembleSource(
		"@rodata\r\n"
		"    let tag: u8[1] = \"\"\r\n"
		"    let value: u32 = 1\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    la r1, value\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto words = r.words();
	const u32 address = (static_cast<u32>(Instruction(words[0]).imm16()) << 16) | Instruction(words[1]).imm16();
	const u32 rodataStart = Memory::UnrestrictedSegmentStart.value() + r.program->header().textSize;

	CHECK_EQ(address, rodataStart + 4);       // one byte of data, three of padding
	CHECK_EQ(address % 4, 0u);
}

// --- Instructions that had no spelling ---------------------------------------------------------

TEST(language, jo_and_jno_read_the_overflow_flag)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    jo forward\r\n"
		"    jno forward\r\n"
		"forward:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK(Instruction(r.words()[0]).opcode() == Opcode::JO);
	CHECK(Instruction(r.words()[1]).opcode() == Opcode::JNO);
}

TEST(language, jo_is_taken_only_when_the_overflow_flag_is_set)
{
	const u32 entry = Memory::UnrestrictedSegmentStart.value();

	// 0x7FFFFFFF + 1 overflows a signed 32-bit add.
	Machine overflowed{
		Instruction::LUI(1, 0x7FFF),
		Instruction::ORI(1, 1, 0xFFFF),
		Instruction::ADDI(2, 1, 1),
		Instruction::JO(i24(16)),
	};
	overflowed.step(4);
	CHECK_EQ(overflowed.pc().value(), entry + 3 * Instruction::Size + 16);

	Machine notOverflowed{
		Instruction::LI(1, 1),
		Instruction::ADDI(2, 1, 1),
		Instruction::JO(i24(16)),
	};
	notOverflowed.step(3);
	CHECK_EQ(notOverflowed.pc().value(), entry + 3 * Instruction::Size);
}

TEST(language, sti_and_cli_drive_the_interrupt_flag)
{
	// User interrupts were unreachable: triggerInterrupt drops numbers >= 16 while the flag is
	// clear, and nothing could set it.
	Machine m{
		Instruction::STI(),
		Instruction::CLI(),
	};

	m.step();
	CHECK(m.flags().interrupt());

	m.step();
	CHECK(!m.flags().interrupt());
}

TEST(language, cli_and_sti_assemble)
{
	AssembleResult r = assembleSource(inText("    sti\r\n    cli"));

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK(Instruction(r.words()[0]).opcode() == Opcode::STI);
	CHECK(Instruction(r.words()[1]).opcode() == Opcode::CLI);
}

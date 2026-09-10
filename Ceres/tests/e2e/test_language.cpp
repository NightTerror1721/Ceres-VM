// Phase 4 language features: constant expressions, data alignment and the instructions that
// were defined in the enum but had no way to be written.

#include "framework.h"
#include "assemble_helper.h"
#include <ceres/vm/ceresvm.h>
#include <ceres/vm/bios.h>
#include <ceres/core/format/memory_map.h>

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

TEST(language, a_constant_can_appear_inside_a_larger_expression)
{
	// The parser used to fold expressions as it read them, which only worked for literals: a
	// constant does not exist until the unit is built. Keeping the tree instead moves the folding
	// to where the symbol table is.
	AssembleResult r = assembleSource(
		"const BASE = 4\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 2 * BASE\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(vm::Instruction(r.words()[0]).imm16(), u16{ 8 });
}

TEST(language, a_constant_can_be_defined_from_another)
{
	AssembleResult r = assembleSource(
		"const BLOCK   = 64\r\n"
		"const HEADER  = 8\r\n"
		"const PAYLOAD = BLOCK - HEADER\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, PAYLOAD\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(vm::Instruction(r.words()[0]).imm16(), u16{ 56 });
}

TEST(language, an_expression_respects_precedence_and_parentheses)
{
	AssembleResult r = assembleSource(
		"const N = 3\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 2 + N * 4\r\n"
		"    li r2, (2 + N) * 4\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(vm::Instruction(r.words()[0]).imm16(), u16{ 14 });
	CHECK_EQ(vm::Instruction(r.words()[1]).imm16(), u16{ 20 });
}

TEST(language, a_constant_referring_to_one_declared_later_is_still_rejected)
{
	// Constants are evaluated in source order, which is what makes a cycle impossible rather than
	// something that has to be detected.
	AssembleResult r = assembleSource(
		"const A = B + 1\r\n"
		"const B = 2\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
}

TEST(language, an_array_size_can_be_computed_from_a_constant)
{
	AssembleResult r = assembleSource(
		"const ROWS = 4\r\n"
		"@bss\r\n"
		"    let grid: u8[ROWS * 2]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, countof(grid)\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(vm::Instruction(r.words()[0]).imm16(), u16{ 8 });
}

TEST(language, sizeof_counts_bytes_and_countof_counts_elements)
{
	AssembleResult r = assembleSource(
		"@bss\r\n"
		"    let values: u32[5]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, countof(values)\r\n"
		"    li r2, sizeof(values)\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(vm::Instruction(r.words()[0]).imm16(), u16{ 5 });
	CHECK_EQ(vm::Instruction(r.words()[1]).imm16(), u16{ 20 });
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

// --- interrupt vector binding -----------------------------------------------------------------

TEST(language, interrupt_binds_a_number_to_a_handler_label)
{
	AssembleResult r = assembleSource(
		"interrupt UserInterrupt0: handler\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n"
		"handler:\r\n"
		"    iret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	auto vectors = r.program->interruptVectors();
	CHECK_EQ(vectors.size(), usize{ 1 });
	if (vectors.empty()) return;

	CHECK_EQ(vectors[0].interruptNumber, u8{ 16 });
	// `ret` is one word, so `handler` starts right after it.
	CHECK_EQ(vectors[0].handlerAddress, MemoryMap::UnrestrictedSegmentStart.value() + Instruction::Size);
}

TEST(language, a_literal_interrupt_number_works_the_same_as_a_name)
{
	AssembleResult r = assembleSource(
		"interrupt 17: handler\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n"
		"handler:\r\n"
		"    iret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	auto vectors = r.program->interruptVectors();
	CHECK_EQ(vectors.size(), usize{ 1 });
	if (!vectors.empty())
		CHECK_EQ(vectors[0].interruptNumber, u8{ 17 });
}

TEST(language, interrupt_0_is_rejected_because_it_is_the_reset_vector)
{
	AssembleResult r = assembleSource(
		"interrupt 0: handler\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n"
		"handler:\r\n"
		"    iret\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("reset vector") != std::string::npos);
}

TEST(language, binding_the_same_interrupt_twice_is_rejected)
{
	AssembleResult r = assembleSource(
		"interrupt UserInterrupt0: first_handler\r\n"
		"interrupt UserInterrupt0: second_handler\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n"
		"first_handler:\r\n"
		"    iret\r\n"
		"second_handler:\r\n"
		"    iret\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("already bound") != std::string::npos);
}

TEST(language, an_interrupt_target_must_be_a_label)
{
	AssembleResult r = assembleSource(
		"const NOT_A_LABEL = 42\r\n"
		"interrupt UserInterrupt0: NOT_A_LABEL\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("must be a label") != std::string::npos);
}

// Multidimensional arrays. The back end never learned about them: a i32[2][3] and a i32[6] produce
// identical bytes, so all of this lives in the type and the parser.
TEST(language, a_two_dimensional_array_is_stored_row_major)
{
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    let grid: i32[2][3] = [[1, 2, 3], [4, 5, 6]]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto data = r.program->data();
	CHECK_EQ(data.size(), usize{ 24 });
	if (data.size() < 24) return;
	for (u32 i = 0; i < 6; ++i)
		CHECK_EQ(static_cast<u32>(data[i * 4]), i + 1);
}

TEST(language, every_dimension_may_be_left_to_the_initialiser)
{
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    let a: i32[][] = [[1, 2, 3], [4, 5, 6]]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, dimof(a, 0)\r\n"
		"    li r2, dimof(a, 1)\r\n"
		"    li r3, countof(a)\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(vm::Instruction(r.words()[0]).imm16(), u16{ 2 });
	CHECK_EQ(vm::Instruction(r.words()[1]).imm16(), u16{ 3 });
	CHECK_EQ(vm::Instruction(r.words()[2]).imm16(), u16{ 6 });
}

TEST(language, an_inner_dimension_may_be_the_inferred_one)
{
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    let a: i32[2][] = [[1, 2, 3], [4, 5, 6]]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, dimof(a, 1)\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(vm::Instruction(r.words()[0]).imm16(), u16{ 3 });
}

// A declared dimension gives something to pad against, so an irregular initialiser is fine there.
// An omitted one does not, and the irregularity is exactly what makes the size unknowable.
TEST(language, a_declared_dimension_pads_a_short_row)
{
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    let a: i32[][3] = [[1, 2], [4, 5, 6]]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, dimof(a, 0)\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(vm::Instruction(r.words()[0]).imm16(), u16{ 2 });

	const auto data = r.program->data();
	CHECK_EQ(data.size(), usize{ 24 });
	if (data.size() < 24) return;
	CHECK_EQ(static_cast<u32>(data[8]), u32{ 0 });  // the padded third element of row 0
	CHECK_EQ(static_cast<u32>(data[12]), u32{ 4 }); // row 1 starts where it should
}

TEST(language, an_irregular_initialiser_cannot_supply_an_omitted_size)
{
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    let a: i32[][] = [[1, 2], [3, 4, 5]]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("dimension 1") != std::string::npos);
}

TEST(language, the_initialiser_must_be_nested_as_deeply_as_the_type)
{
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    let a: i32[][] = [1, 2, 3]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
}

TEST(language, a_flat_initialiser_works_when_every_size_is_written_down)
{
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    let a: i32[2][3] = [1, 2, 3, 4, 5, 6]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(r.program->data().size(), usize{ 24 });
}

TEST(language, without_an_initialiser_every_dimension_needs_a_size)
{
	AssembleResult r = assembleSource(
		"@bss\r\n"
		"    let a: u8[][16]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
}

// A string inside an array fills a row rather than being one element, which is what makes a table
// of fixed-width strings expressible at all.
TEST(language, a_string_inside_an_array_fills_a_row)
{
	AssembleResult r = assembleSource(
		"@rodata\r\n"
		"    let names: u8[][8] = [\"ada\", \"grace\"]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, dimof(names, 0)\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(vm::Instruction(r.words()[0]).imm16(), u16{ 2 });

	const auto rodata = r.program->rodata();
	CHECK_EQ(rodata.size(), usize{ 16 });
	if (rodata.size() < 16) return;
	CHECK_EQ(static_cast<char>(rodata[0]), 'a');
	CHECK_EQ(static_cast<u32>(rodata[4]), u32{ 0 }); // "ada" plus its zero, then padding
	CHECK_EQ(static_cast<char>(rodata[8]), 'g');     // the second row starts on its own boundary
}

TEST(language, a_three_dimensional_array_is_accepted)
{
	AssembleResult r = assembleSource(
		"@bss\r\n"
		"    let cube: i16[4][4][4]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, countof(cube)\r\n"
		"    li r2, dimof(cube, 2)\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(vm::Instruction(r.words()[0]).imm16(), u16{ 64 });
	CHECK_EQ(vm::Instruction(r.words()[1]).imm16(), u16{ 4 });
}

// The `string` alias is `u8[]` spelled differently, so it must take the same road: a size left
// to the initialiser, not one written down as zero. Regression: the DataType -> DataTypeReference
// mapping used to confuse the unsized array with a written size of 0 ("Array size cannot be 0").
TEST(language, the_string_alias_infers_its_size_from_the_initialiser)
{
	AssembleResult r = assembleSource(
		"@rodata\r\n"
		"    let greeting: string = \"hola\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, countof(greeting)\r\n"
		"    li r2, sizeof(greeting)\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(vm::Instruction(r.words()[0]).imm16(), u16{ 5 });
	CHECK_EQ(vm::Instruction(r.words()[1]).imm16(), u16{ 5 });
}

// Type aliases. Each resolves to an underlying scalar, but the spelling is kept: it is what
// diagnostics say, and for three of them it is what makes a range check possible.
TEST(language, ptr_is_a_u32_that_reads_as_an_address)
{
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    let head: ptr    = 0\r\n"
		"    let table: ptr[4]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ldv r1, head\r\n"
		"    li  r2, sizeof(table)\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	// A ptr loads as a full word, exactly as a u32 does. One word, because the variable is close
	// enough for the linker to relax the ldv into its PC-relative form.
	CHECK_EQ(Instruction(r.words()[0]).opcode() == Opcode::LDRP, true);
	CHECK_EQ(Instruction(r.words()[1]).imm16(), u16{ 16 });
}

TEST(language, the_machine_vocabulary_aliases_are_accepted)
{
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    let a: byte = 1\r\n"
		"    let b: half = 2\r\n"
		"    let c: word = 3\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, sizeof(a)\r\n"
		"    li r2, sizeof(b)\r\n"
		"    li r3, sizeof(c)\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 1 });
	CHECK_EQ(Instruction(r.words()[1]).imm16(), u16{ 2 });
	CHECK_EQ(Instruction(r.words()[2]).imm16(), u16{ 4 });
}

// The interrupt vector table has 64 entries and a bool has two values. Nothing about u8 says so.
TEST(language, an_alias_that_promises_a_range_enforces_it)
{
	AssembleResult ok = assembleSource(
		"@data\r\n"
		"    let vector: irq  = 16\r\n"
		"    let channel: port = 0x01\r\n"
		"    let flag:   bool = true\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");
	CHECK(ok.ok());
	if (!ok.ok()) Registry::instance().recordFailure(ok.joinedErrors());

	AssembleResult tooBig = assembleSource(
		"@data\r\n"
		"    let vector: irq = 99\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");
	CHECK(!tooBig.ok());
	CHECK(tooBig.joinedErrors().find("irq") != std::string::npos);

	AssembleResult notABool = assembleSource(
		"@data\r\n"
		"    let flag: bool = 5\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");
	CHECK(!notABool.ok());
	CHECK(notABool.joinedErrors().find("bool") != std::string::npos);
}

TEST(language, an_array_of_a_bounded_alias_is_checked_element_by_element)
{
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    let vectors: irq[3] = [1, 2, 90]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("90") != std::string::npos);
}

// TokenType::LiteralBool and its factory both existed and nothing ever produced one.
TEST(language, true_and_false_are_boolean_literals)
{
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    let yes: bool = true\r\n"
		"    let no:  bool = false\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto data = r.program->data();
	CHECK(data.size() >= 2);
	if (data.size() < 2) return;
	CHECK_EQ(static_cast<u32>(data[0]), u32{ 1 });
	CHECK_EQ(static_cast<u32>(data[1]), u32{ 0 });
}

// --- Register aliases -----------------------------------------------------------------------

// Purely lexical, and resolved in the parser: by the time anything downstream sees the operand it
// is an ordinary register, so nothing else in the pipeline has to know registers can be named.
TEST(language, a_register_alias_stands_in_for_its_register)
{
	AssembleResult r = assembleSource(
		"alias cursor = r5\r\n"
		"alias total  = r6\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    clr  total\r\n"
		"    ldrb total, [cursor + 1]\r\n"
		"    add  total, total, cursor\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(Instruction(r.words()[0]).rd(), u8{ 6 });
	CHECK_EQ(Instruction(r.words()[1]).rd(), u8{ 6 });
	CHECK_EQ(Instruction(r.words()[1]).rs(), u8{ 5 }); // the alias works as a memory base too
	CHECK_EQ(Instruction(r.words()[2]).rt(), u8{ 5 });
}

TEST(language, an_alias_cannot_shadow_a_register_or_itself)
{
	CHECK(!assembleSource("alias r5 = r6\r\n@text\r\nglobal main:\r\n    ret\r\n").ok());
	CHECK(!assembleSource("alias a = r1\r\nalias a = r2\r\n@text\r\nglobal main:\r\n    ret\r\n").ok());
	CHECK(!assembleSource("alias a = nope\r\n@text\r\nglobal main:\r\n    ret\r\n").ok());
}

// --- Structs --------------------------------------------------------------------------------

// A struct declares no storage: it defines one constant per field offset plus its own name for the
// total size, which is all `[r1 + Entity.y]` and `u8[32][Entity]` actually need.
TEST(language, a_struct_lays_its_fields_out_with_alignment)
{
	AssembleResult r = assembleSource(
		"struct Entity\r\n"
		"    x:      i32\r\n"
		"    y:      i32\r\n"
		"    health: u16\r\n"
		"    flags:  u8\r\n"
		"endstruct\r\n"
		"@bss\r\n"
		"    let player: u8[Entity]\r\n"
		"    let mobs:   u8[32][Entity]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, Entity.x\r\n"
		"    li r2, Entity.y\r\n"
		"    li r3, Entity.health\r\n"
		"    li r4, Entity.flags\r\n"
		"    li r5, Entity\r\n"
		"    li r6, sizeof(mobs)\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 0 });
	CHECK_EQ(Instruction(r.words()[1]).imm16(), u16{ 4 });
	CHECK_EQ(Instruction(r.words()[2]).imm16(), u16{ 8 });
	CHECK_EQ(Instruction(r.words()[3]).imm16(), u16{ 10 });
	// 11 bytes of fields, rounded up to the widest one so an array of them stays aligned.
	CHECK_EQ(Instruction(r.words()[4]).imm16(), u16{ 12 });
	CHECK_EQ(Instruction(r.words()[5]).imm16(), u16{ 384 });
}

TEST(language, a_struct_field_offset_works_as_a_memory_displacement)
{
	AssembleResult r = assembleSource(
		"struct Point\r\n"
		"    x: i32\r\n"
		"    y: i32\r\n"
		"endstruct\r\n"
		"@bss\r\n"
		"    let p: u8[Point]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    la  r1, p\r\n"
		"    ldr r2, [r1 + Point.y]\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(Instruction(r.words()[2]).opcode() == Opcode::LDR, true);
	CHECK_EQ(Instruction(r.words()[2]).imm16(), u16{ 4 });
}

// A byte array dimensioned by a struct initialises positionally, in field order: each value is
// coerced to its field's type and padding is zero-filled, so the emitter still sees plain bytes.
TEST(language, a_struct_initialiser_maps_values_to_fields_in_order)
{
	AssembleResult r = assembleSource(
		"struct Entity\r\n"
		"    x:      i32\r\n"
		"    y:      i32\r\n"
		"    health: u16\r\n"
		"    flags:  u8\r\n"
		"endstruct\r\n"
		"@data\r\n"
		"    let player: u8[Entity] = [10, 20, 100, 1]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto data = r.program->data();
	const u8 expected[] = { 10, 0, 0, 0, 20, 0, 0, 0, 100, 0, 1, 0 };
	CHECK_EQ(data.size(), sizeof(expected));
	for (usize i = 0; i < sizeof(expected); ++i)
		CHECK_EQ(data[i], expected[i]);
}

TEST(language, a_short_struct_initialiser_zero_fills_the_remaining_fields)
{
	AssembleResult r = assembleSource(
		"struct Entity\r\n"
		"    x:      i32\r\n"
		"    y:      i32\r\n"
		"    health: u16\r\n"
		"    flags:  u8\r\n"
		"endstruct\r\n"
		"@data\r\n"
		"    let player: u8[Entity] = [7]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto data = r.program->data();
	const u8 expected[] = { 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	CHECK_EQ(data.size(), sizeof(expected));
	for (usize i = 0; i < sizeof(expected); ++i)
		CHECK_EQ(data[i], expected[i]);
}

TEST(language, a_struct_initialiser_with_too_many_values_is_rejected)
{
	AssembleResult r = assembleSource(
		"struct Entity\r\n"
		"    x: i32\r\n"
		"    y: i32\r\n"
		"endstruct\r\n"
		"@data\r\n"
		"    let player: u8[Entity] = [1, 2, 3]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
}

TEST(language, a_struct_initialiser_checks_each_value_against_its_field_type)
{
	AssembleResult r = assembleSource(
		"struct S\r\n"
		"    a: u16\r\n"
		"endstruct\r\n"
		"@data\r\n"
		"    let v: u8[S] = [70000]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
}

TEST(language, a_struct_array_field_initialises_from_a_nested_group)
{
	AssembleResult r = assembleSource(
		"struct Tile\r\n"
		"    corners: i16[4]\r\n"
		"    id:      u32\r\n"
		"endstruct\r\n"
		"@data\r\n"
		"    let t: u8[Tile] = [[1, 2, 3, 4], 99]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto data = r.program->data();
	const u8 expected[] = { 1, 0, 2, 0, 3, 0, 4, 0, 99, 0, 0, 0 };
	CHECK_EQ(data.size(), sizeof(expected));
	for (usize i = 0; i < sizeof(expected); ++i)
		CHECK_EQ(data[i], expected[i]);
}

TEST(language, a_struct_used_only_through_an_initialiser_is_not_reported_as_unused)
{
	AssembleResult r = assembleSource(
		"struct Entity\r\n"
		"    x: i32\r\n"
		"    y: i32\r\n"
		"endstruct\r\n"
		"@data\r\n"
		"    let player: u8[Entity] = [1, 2]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    la r1, player\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK(r.joinedWarnings().find("Entity") == std::string::npos);
}

TEST(language, an_array_of_structs_initialises_one_group_per_instance)
{
	AssembleResult r = assembleSource(
		"struct Point\r\n"
		"    x: i32\r\n"
		"    y: i32\r\n"
		"endstruct\r\n"
		"@data\r\n"
		"    let pts: u8[2][Point] = [[1, 2], [3, 4]]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto data = r.program->data();
	const u8 expected[] = { 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0 };
	CHECK_EQ(data.size(), sizeof(expected));
	for (usize i = 0; i < sizeof(expected); ++i)
		CHECK_EQ(data[i], expected[i]);
}

// --- Unused private declarations --------------------------------------------------------------

// `global` created a category the language did not have: a declaration that provably cannot be
// reached from anywhere else, so one nobody names in its own file is dead with certainty.
TEST(language, an_unused_private_declaration_is_a_warning_not_an_error)
{
	AssembleResult r = assembleSource(
		"const UNUSED = 4\r\n"
		"const USED   = 7\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, USED\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK(r.joinedWarnings().find("UNUSED") != std::string::npos);
	CHECK(r.joinedWarnings().find("USED,") == std::string::npos);
}

TEST(language, an_unused_global_declaration_is_not_warned_about)
{
	// It is exported, so something outside this file may well be using it.
	AssembleResult r = assembleSource(
		"global const EXPORTED = 4\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	CHECK_EQ(r.warnings.size(), usize{ 0 });
}

TEST(language, an_unused_private_macro_is_warned_about)
{
	AssembleResult r = assembleSource(
		"macro never_called\r\n"
		"    nop\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	CHECK(r.joinedWarnings().find("never_called") != std::string::npos);
}

TEST(language, a_string_initialiser_for_a_struct_sized_array_is_bytes_not_fields)
{
	// The struct path reads a flat list as one value per field. A string is flattened to
	// characters before anything sees it, so without a marker `"abc"` would land as 'a' in the
	// first i32, 'b' in the second and the terminating zero in whatever came after - silently.
	AssembleResult r = assembleSource(
		"struct Entity\r\n"
		"    x: i32\r\n"
		"    y: i32\r\n"
		"    health: u16\r\n"
		"    flags: u8\r\n"
		"endstruct\r\n"
		"@data\r\n"
		"    global let s: u8[Entity] = \"abc\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto& data = r.program->data();
	CHECK_EQ(data.size(), usize{ 12 });
	if (data.size() < 4) return;
	CHECK_EQ(data[0], u8{ 'a' });
	CHECK_EQ(data[1], u8{ 'b' });
	CHECK_EQ(data[2], u8{ 'c' });
	CHECK_EQ(data[3], u8{ 0 });
}

TEST(language, a_struct_initialiser_takes_at_most_one_instance_count)
{
	// `u8[2][2][Pair]` multiplied the counts and wanted a flat run of four instances, where an
	// ordinary `i32[2][2]` wants them nested. The same shape meaning two different things is
	// worse than not accepting it.
	AssembleResult r = assembleSource(
		"struct Pair\r\n"
		"    a: i32\r\n"
		"    b: i32\r\n"
		"endstruct\r\n"
		"@data\r\n"
		"    global let grid: u8[2][2][Pair] = [[1,2],[3,4],[5,6],[7,8]]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("at most one instance count") != std::string::npos);
}

TEST(language, a_struct_name_is_a_type)
{
	AssembleResult r = assembleSource(
		"struct Entity\r\n"
		"    x: i32\r\n"
		"    y: i32\r\n"
		"    health: u16\r\n"
		"    flags: u8\r\n"
		"endstruct\r\n"
		"@data\r\n"
		"    global let player: Entity    = [10, 20, 100, 1]\r\n"
		"    global let mobs:   Entity[2] = [[1,2,3,4],[5,6,7,8]]\r\n"
		"    global let old:    u8[Entity] = [10, 20, 100, 1]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	// Entity is exactly u8[Entity]: twelve bytes, and the long spelling produces the same ones.
	const auto& data = r.program->data();
	CHECK_EQ(data.size(), usize{ 12 + 24 + 12 });
	if (data.size() < 48) return;
	for (usize i = 0; i < 12; ++i)
		CHECK_EQ(data[i], data[36 + i]);
}

TEST(language, a_struct_typed_field_is_initialised_as_a_struct)
{
	// The field's type resolves to plain u8 bytes like any other, so without knowing which struct
	// it came from `[1, 2]` would write the bytes 1 and 2 instead of two i32s.
	AssembleResult r = assembleSource(
		"struct Point\r\n"
		"    x: i32\r\n"
		"    y: i32\r\n"
		"endstruct\r\n"
		"struct Line\r\n"
		"    a: Point\r\n"
		"    b: Point\r\n"
		"endstruct\r\n"
		"@data\r\n"
		"    global let l: Line = [[1, 2], [3, 4]]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto& data = r.program->data();
	CHECK_EQ(data.size(), usize{ 16 });
	if (data.size() < 16) return;
	CHECK_EQ(data[0], u8{ 1 });
	CHECK_EQ(data[1], u8{ 0 });  // an i32, not a byte
	CHECK_EQ(data[4], u8{ 2 });
	CHECK_EQ(data[8], u8{ 3 });
	CHECK_EQ(data[12], u8{ 4 });
}

TEST(language, only_a_struct_can_be_written_as_a_type)
{
	AssembleResult r = assembleSource(
		"const MAX_PLAYERS = 4\r\n"
		"@bss\r\n"
		"    global let p: MAX_PLAYERS\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("is not a struct") != std::string::npos);
}

// --- align, org and assert -------------------------------------------------------------------

TEST(language, align_and_org_place_what_follows_them)
{
	AssembleResult r = assembleSource(
		"@data\r\n"
		"    global let a: u8 = 1\r\n"
		"    align 16\r\n"
		"    global let b: u8 = 2\r\n"
		"    org 32\r\n"
		"    global let c: u8 = 3\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto& data = r.program->data();
	CHECK_EQ(data.size(), usize{ 36 }); // 32 + one byte, padded to four
	if (data.size() < 33) return;
	CHECK_EQ(data[0], u8{ 1 });
	CHECK_EQ(data[16], u8{ 2 });  // align 16
	CHECK_EQ(data[32], u8{ 3 });  // org 32
	CHECK_EQ(data[1], u8{ 0 });   // and the gap is zero
}

TEST(language, assert_checks_a_constant_expression_at_assembly_time)
{
	AssembleResult ok = assembleSource(
		"struct Frame\r\n"
		"    saved: u32\r\n"
		"    count: u32\r\n"
		"endstruct\r\n"
		"assert Frame % 4 == 0\r\n"
		"assert Frame >= 8\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");
	CHECK(ok.ok());
	if (!ok.ok()) { Registry::instance().recordFailure(ok.joinedErrors()); return; }

	AssembleResult bad = assembleSource(
		"const N = 5\r\n"
		"assert N % 4 == 0, \"N has to be a multiple of four\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");
	CHECK(!bad.ok());
	CHECK(bad.joinedErrors().find("N has to be a multiple of four") != std::string::npos);
}

TEST(language, align_and_org_refuse_what_they_cannot_do)
{
	AssembleResult notPowerOfTwo = assembleSource(
		"@data\r\n"
		"    global let a: u8 = 1\r\n"
		"    align 6\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");
	CHECK(!notPowerOfTwo.ok());
	CHECK(notPowerOfTwo.joinedErrors().find("power of two") != std::string::npos);

	// Sections are placed by the linker, so org is an offset within one - and it cannot go back
	// over what is already there.
	AssembleResult backwards = assembleSource(
		"@data\r\n"
		"    global let a: u32 = 1\r\n"
		"    org 2\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");
	CHECK(!backwards.ok());
	CHECK(backwards.joinedErrors().find("would move backwards") != std::string::npos);
}

TEST(language, a_constant_expression_can_compare_and_take_a_remainder)
{
	AssembleResult r = assembleSource(
		"const A = 10 % 4\r\n"
		"const B = 3 < 4\r\n"
		"const C = 3 == 4\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, A\r\n"
		"    li r2, B\r\n"
		"    li r3, C\r\n");
	auto words = r.words();

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(Instruction(words[0]).imm16(), u16{ 2 });
	CHECK_EQ(Instruction(words[1]).imm16(), u16{ 1 });
	CHECK_EQ(Instruction(words[2]).imm16(), u16{ 0 });
}

TEST(language, a_string_or_a_float_can_be_written_where_an_operand_goes)
{
	// Neither fits in an instruction: a string is a run of bytes and a float needs 32 bits. Both
	// become an anonymous .rodata declaration, and the operand becomes its address.
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    la  r1, \"listo\"\r\n"
		"    ldv f1, 1.5\r\n"
		"    ret\r\n");
	auto words = r.words();

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	// The string's address is built the way any other address is.
	CHECK_EQ(Instruction(words[0]).opcode() == Opcode::LUI, true);
	CHECK_EQ(Instruction(words[1]).opcode() == Opcode::ORI, true);
	// And the float is close enough to read PC-relatively, in one word.
	CHECK_EQ(Instruction(words[2]).opcode() == Opcode::FLDRP, true);

	// "listo" and its terminator, padded, then the four bytes of the float.
	const auto& rodata = r.program->rodata();
	CHECK_EQ(rodata.size(), usize{ 12 });
	if (rodata.size() < 6) return;
	CHECK_EQ(rodata[0], u8{ 'l' });
	CHECK_EQ(rodata[5], u8{ 0 });
}

TEST(pipeline_like, an_anonymous_string_prints)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    la r1, \"listo\"\r\n"
		"    li r2, 5\r\n"
		"    outm 0x01, r1, r2\r\n"
		"    ret\r\n");
	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
}

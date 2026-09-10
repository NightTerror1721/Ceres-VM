// Macros: declaration, expansion, parameter substitution and label hygiene.

#include "framework.h"
#include "assemble_helper.h"
#include <ceres/vm/ceresvm.h>
#include <ceres/devices/devices.h>
#include <ceres/core/format/memory_map.h>
#include <string>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::devices;
using namespace ceres::testing;

namespace
{
	class CapturingTerminal final : public IODevice
	{
	private:
		std::string _output;

	public:
		const std::string& output() const noexcept { return _output; }

		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Terminal, *this);
		}

		u8 readUnsignedByte(Address) override { return 0; }
		i8 readSignedByte(Address) override { return 0; }
		u16 readUnsignedHalfword(Address) override { return 0; }
		i16 readSignedHalfword(Address) override { return 0; }
		u32 readUnsignedWord(Address) override { return 0; }

		void writeByte(Address offset, u8 value) override
		{
			if (offset == TerminalDevice::OutputRegister)
				_output.push_back(static_cast<char>(value));
		}
		void writeHalfword(Address offset, u16 value) override { writeByte(offset, static_cast<u8>(value)); }
		void writeWord(Address offset, u32 value) override { writeByte(offset, static_cast<u8>(value)); }
	};

	struct RunResult
	{
		bool assembled = false;
		std::string errors;
		std::string output;
	};

	RunResult assembleAndRun(std::string_view source)
	{
		RunResult result;

		AssembleResult assembled = assembleSource(source, "macros");
		if (!assembled.ok())
		{
			result.errors = assembled.joinedErrors();
			return result;
		}
		result.assembled = true;

		CeresVM vm{};
		SystemControlDevice sysctl{ [&vm]() { vm.shutdown(); }, [&vm]() { vm.shutdown(); } };
		sysctl.attachTo(vm.io());

		CapturingTerminal terminal{};
		terminal.attachTo(vm.io());

		if (auto loaded = vm.loadProgram(assembled.program.value()); !loaded)
		{
			result.errors = loaded.error();
			result.assembled = false;
			return result;
		}

		(void)vm.run();
		result.output = terminal.output();
		return result;
	}

	constexpr std::string_view shutdown =
		"    li r0, 1\r\n"
		"    la r13, 0xFFFF0000\r\n"
		"    strb [r13 + 0], r0\r\n";
}

TEST(macros, a_macro_without_parameters_expands_in_place)
{
	AssembleResult r = assembleSource(
		"macro two_nops\r\n"
		"    nop\r\n"
		"    nop\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    two_nops\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	// Two NOPs and a RET: the macro contributed real instructions, not a placeholder.
	CHECK_EQ(r.words().size(), 3u);
	CHECK(Instruction(r.words()[0]).opcode() == Opcode::NOP);
	CHECK(Instruction(r.words()[1]).opcode() == Opcode::NOP);
	CHECK(Instruction(r.words()[2]).opcode() == Opcode::RET);
}

TEST(macros, parameters_are_substituted_positionally)
{
	AssembleResult r = assembleSource(
		"macro load_pair $first, $second\r\n"
		"    li $first, 11\r\n"
		"    li $second, 22\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    load_pair r4, r7\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto words = r.words();
	CHECK_EQ(words.size(), 3u);
	CHECK_EQ(Instruction(words[0]).rd(), u8{ 4 });
	CHECK_EQ(Instruction(words[0]).imm16(), u16{ 11 });
	CHECK_EQ(Instruction(words[1]).rd(), u8{ 7 });
	CHECK_EQ(Instruction(words[1]).imm16(), u16{ 22 });
}

TEST(macros, an_immediate_argument_reaches_the_encoding)
{
	AssembleResult r = assembleSource(
		"macro set $reg, $value\r\n"
		"    li $reg, $value\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    set r2, 1234\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 1234 });
}

TEST(macros, labels_are_hygienic_across_two_uses_of_the_same_macro)
{
	// The whole point of %%labels: using a macro twice in the same scope must not redefine its
	// own internal label.
	AssembleResult r = assembleSource(
		"macro skip_next $reg\r\n"
		"    cmp $reg, 0\r\n"
		"    jz %%done\r\n"
		"    nop\r\n"
		"%%done:\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    skip_next r1\r\n"
		"    skip_next r2\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto words = r.words();
	CHECK_EQ(words.size(), 7u);   // (cmp, jz, nop) twice, then ret

	// Each JZ must skip its own NOP, not the other expansion's.
	CHECK_EQ(Instruction(words[1]).simm24().signedValue(), 2 * static_cast<i32>(Instruction::Size));
	CHECK_EQ(Instruction(words[4]).simm24().signedValue(), 2 * static_cast<i32>(Instruction::Size));
}

TEST(macros, a_macro_can_call_another_macro)
{
	AssembleResult r = assembleSource(
		"macro inner $reg\r\n"
		"    li $reg, 5\r\n"
		"endmacro\r\n"
		"macro outer $reg\r\n"
		"    inner $reg\r\n"
		"    add $reg, $reg, 1\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    outer r3\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto words = r.words();
	CHECK_EQ(words.size(), 3u);
	CHECK(Instruction(words[0]).opcode() == Opcode::LI);
	CHECK_EQ(Instruction(words[0]).rd(), u8{ 3 });
	CHECK(Instruction(words[1]).opcode() == Opcode::ADDI);
}

TEST(macros, overloading_by_argument_count_picks_the_right_body)
{
	// Macros are keyed by name and arity, so two macros may share a name.
	AssembleResult r = assembleSource(
		"macro emit $a\r\n"
		"    li $a, 1\r\n"
		"endmacro\r\n"
		"macro emit $a, $b\r\n"
		"    li $a, 2\r\n"
		"    li $b, 3\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    emit r1\r\n"
		"    emit r2, r3\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const auto words = r.words();
	CHECK_EQ(words.size(), 4u);
	CHECK_EQ(Instruction(words[0]).imm16(), u16{ 1 });
	CHECK_EQ(Instruction(words[1]).imm16(), u16{ 2 });
	CHECK_EQ(Instruction(words[2]).imm16(), u16{ 3 });
}

TEST(macros, a_call_with_the_wrong_argument_count_is_reported)
{
	AssembleResult r = assembleSource(
		"macro needs_two $a, $b\r\n"
		"    li $a, 1\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    needs_two r1\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("Unknown mnemonic or macro") != std::string::npos);
}

TEST(macros, a_recursive_macro_is_reported_instead_of_hanging)
{
	AssembleResult r = assembleSource(
		"macro forever $a\r\n"
		"    forever $a\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    forever r1\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("recursive") != std::string::npos);
}

TEST(macros, a_macro_label_outside_a_macro_body_is_reported)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"%%stray:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
}

TEST(macros, an_unterminated_macro_is_reported)
{
	AssembleResult r = assembleSource(
		"macro never_closed $a\r\n"
		"    li $a, 1\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("endmacro") != std::string::npos);
}

TEST(macros, an_expanded_macro_runs)
{
	// End to end: the expansion has to survive layout, linking and execution.
	RunResult r = assembleAndRun(std::format(
		"macro print_char $reg, $code\r\n"
		"    li $reg, $code\r\n"
		"    la r13, 0xFF000004\r\n"
		"    strb [r13 + 0], $reg\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    print_char r1, 72\r\n"
		"    print_char r2, 105\r\n"
		"{}", shutdown));

	CHECK(r.assembled);
	if (!r.assembled) { Registry::instance().recordFailure(r.errors); return; }
	CHECK_EQ(r.output, std::string{ "Hi" });
}

TEST(macros, a_hygienic_loop_inside_a_macro_runs_twice_independently)
{
	RunResult r = assembleAndRun(std::format(
		"macro count_down $reg, $from, $char\r\n"
		"    li $reg, $from\r\n"
		"%%loop:\r\n"
		"    li r9, $char\r\n"
		"    la r13, 0xFF000004\r\n"
		"    strb [r13 + 0], r9\r\n"
		"    sub $reg, $reg, 1\r\n"
		"    cmp $reg, 0\r\n"
		"    jnz %%loop\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    count_down r1, 2, 65\r\n"
		"    count_down r2, 3, 66\r\n"
		"{}", shutdown));

	CHECK(r.assembled);
	if (!r.assembled) { Registry::instance().recordFailure(r.errors); return; }
	CHECK_EQ(r.output, std::string{ "AABBB" });
}

// --- A parameter inside the brackets ------------------------------------------------------------
//
// The body of a macro is parsed once, where it is written, so `[$base + 4]` has to parse before
// anyone knows which register `$base` is. What the argument decides is filled in at expansion, and
// only that: the brackets, the displacement and the access type were all decided by the source.

TEST(macros, a_parameter_can_be_the_base_of_a_memory_operand)
{
	AssembleResult r = assembleSource(
		"macro load_at $dst, $base\r\n"
		"    ldr $dst, [$base + 4]\r\n"
		"endmacro\r\n"
		"macro store_at $base, $src\r\n"
		"    str [$base + 8], $src\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    load_at r1, r2\r\n"
		"    store_at r2, r1\r\n"
		"    ret\r\n",
		"macroparam");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const Instruction load{ r.words()[0] };
	CHECK(load.opcode() == Opcode::LDR);
	CHECK_EQ(load.rd(), u8{ 1 });
	CHECK_EQ(load.rs(), u8{ 2 });
	CHECK_EQ(load.simm16(), i16{ 4 });

	const Instruction store{ r.words()[1] };
	CHECK(store.opcode() == Opcode::STR);
	CHECK_EQ(store.rd(), u8{ 2 });
	CHECK_EQ(store.simm16(), i16{ 8 });
}

TEST(macros, what_a_parameter_offset_means_is_decided_by_the_argument)
{
	// The same line of the same macro is a displacement, an index or a symbolic offset depending
	// on what it is handed - exactly as if the three had been written out by hand.
	AssembleResult r = assembleSource(
		"const SLOT = 12\r\n"
		"macro load_at $base, $off\r\n"
		"    ldr r1, [$base + $off]\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    load_at r2, 8\r\n"
		"    load_at r2, r3\r\n"
		"    load_at r2, SLOT\r\n"
		"    ret\r\n",
		"macroparamoffset");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	const Instruction displacement{ r.words()[0] };
	CHECK(displacement.opcode() == Opcode::LDR);
	CHECK_EQ(displacement.simm16(), i16{ 8 });

	// A register turns it into an index, which is a different opcode.
	const Instruction indexed{ r.words()[1] };
	CHECK(indexed.opcode() == Opcode::LDRX);
	CHECK_EQ(indexed.rs(), u8{ 2 });
	CHECK_EQ(indexed.rt(), u8{ 3 });

	const Instruction symbolic{ r.words()[2] };
	CHECK(symbolic.opcode() == Opcode::LDR);
	CHECK_EQ(symbolic.simm16(), i16{ 12 });
}

TEST(macros, an_access_type_still_picks_the_width_around_a_parameter)
{
	AssembleResult r = assembleSource(
		"macro load_byte $base\r\n"
		"    ldr r1, u8[$base + 4]\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    load_byte r2\r\n"
		"    ret\r\n",
		"macroparamtyped");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK(Instruction(r.words()[0]).opcode() == Opcode::LDRB);
}

TEST(macros, a_base_that_is_not_a_register_is_reported_against_the_macro)
{
	// The message has to name the parameter and the macro: the line that fails is inside the
	// macro body, and the mistake is at the call site.
	AssembleResult r = assembleSource(
		"macro load_at $base\r\n"
		"    ldr r1, [$base + 4]\r\n"
		"endmacro\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    load_at 42\r\n"
		"    ret\r\n",
		"macroparambad");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("$base") != std::string::npos);
	CHECK(r.joinedErrors().find("load_at") != std::string::npos);
}

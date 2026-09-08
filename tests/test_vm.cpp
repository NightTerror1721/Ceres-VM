// VM round-trips: build an instruction with the factories, execute it, inspect the machine.
//
// These bypass the assembler entirely, so a failure here means the machine itself decodes or
// executes a field wrongly — which is exactly the class of defect SE-04 turned out to be.

#include "framework.h"
#include "vm/ceresvm.h"
#include "vm/disassembler.h"
#include <vector>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::testing;

namespace
{
	// A machine loaded with a handful of instructions at the start of the unrestricted segment,
	// stepped one instruction at a time.
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

			// reset() takes the initial PC from the reset vector at address 0.
			_vm.memory().writeUnchecked<u32>(0_addr, entry.value());
			_vm.engine().reset();
		}

		void step(usize count = 1)
		{
			for (usize i = 0; i < count; ++i)
				_vm.engine().step();
		}

		u32 reg(usize index) const { return _vm.engine().registers().getValue(index); }
		f32 freg(usize index) const { return _vm.engine().fregisters().getValue(index); }
		const FlagRegister& flags() const { return _vm.engine().flags(); }
		Address pc() const { return _vm.engine().programCounter(); }


		Memory& memory() { return _vm.memory(); }
		ExecutionEngine& engine() { return _vm.engine(); }

	};
}

TEST(vm, li_loads_an_immediate)
{
	Machine m{ Instruction::LI(4, 4321) };
	m.step();

	CHECK_EQ(m.reg(4), 4321u);
	CHECK_EQ(m.pc().value(), Memory::UnrestrictedSegmentStart.value() + Instruction::Size);
}

TEST(vm, add_sets_zero_flag_when_the_result_is_zero)
{
	Machine m{
		Instruction::LI(1, 0),
		Instruction::LI(2, 0),
		Instruction::ADD(3, 1, 2),
	};
	m.step(3);

	CHECK_EQ(m.reg(3), 0u);
	CHECK(m.flags().zero());
}

TEST(vm, sub_sets_carry_on_borrow)
{
	Machine m{
		Instruction::LI(1, 1),
		Instruction::LI(2, 2),
		Instruction::SUB(3, 1, 2),
	};
	m.step(3);

	CHECK_EQ(m.reg(3), 0xFFFFFFFFu);
	CHECK(m.flags().carry());
	CHECK(m.flags().sign());
}

// --- SE-04: a store must read its base from Rd and its value from Rs -----------------------

TEST(vm, store_and_load_round_trip_through_memory)
{
	const u32 base = Memory::UnrestrictedSegmentStart.value() + 0x100;

	Machine m{
		Instruction::LUI(1, static_cast<u16>(base >> 16)),
		Instruction::ORI(1, 1, static_cast<u16>(base & 0xFFFF)),
		Instruction::LI(2, 0xBEEF),
		Instruction::STRH(1, 2, 4),      // [r1 + 4] = r2 (halfword)
		Instruction::LDRH(3, 1, 4),      // r3 = [r1 + 4]
	};
	m.step(5);

	CHECK_EQ(m.reg(1), base);
	CHECK_EQ(m.reg(3), 0xBEEFu);
	CHECK_EQ(m.memory().read<u16>(Address(base + 4)), u16{ 0xBEEF });
}

TEST(vm, a_store_displacement_with_a_high_nibble_is_not_read_as_a_register)
{
	// 0x70F0 as a displacement: under the old encoding its top nibble was decoded as the value
	// register index, so this wrote from r7 instead of r2. Still positive as a signed field, which
	// is what keeps this a decoding test rather than a sign test.
	const u32 base = Memory::UnrestrictedSegmentStart.value();

	Machine m{
		Instruction::LUI(1, static_cast<u16>(base >> 16)),
		Instruction::ORI(1, 1, static_cast<u16>(base & 0xFFFF)),
		Instruction::LI(2, 0x1234),
		Instruction::STRH(1, 2, 0x70F0),
	};
	m.step(4);

	CHECK_EQ(m.memory().read<u16>(Address(base + 0x70F0)), u16{ 0x1234 });
}

// A displacement is a *signed* 16-bit field. It used to be zero-extended, so `[base - 8]` reached
// 65528 bytes above the base instead of eight below it - which made a frame pointer, and with it any
// calling convention, impossible to address downward.
TEST(vm, a_negative_displacement_reaches_below_the_base)
{
	const u32 base = Memory::UnrestrictedSegmentStart.value() + 64;

	Machine m{
		Instruction::LUI(1, static_cast<u16>(base >> 16)),
		Instruction::ORI(1, 1, static_cast<u16>(base & 0xFFFF)),
		Instruction::LI(2, 0xBEEF),
		Instruction::STR(1, 2, static_cast<u16>(-8)),      // *(u32*)(r1 - 8) = r2
		Instruction::LDR(3, 1, static_cast<u16>(-8)),      // r3 = *(u32*)(r1 - 8)
		Instruction::LEA(4, 1, static_cast<u16>(-8)),      // r4 = r1 - 8
	};
	m.step(6);

	CHECK_EQ(m.memory().read<u32>(Address(base - 8)), u32{ 0xBEEF });
	CHECK_EQ(m.reg(3), 0xBEEFu);
	CHECK_EQ(m.reg(4), base - 8);
}

TEST(vm, the_displacement_range_runs_both_ways)
{
	const u32 base = Memory::UnrestrictedSegmentStart.value() + 40000;

	Machine m{
		Instruction::LUI(1, static_cast<u16>(base >> 16)),
		Instruction::ORI(1, 1, static_cast<u16>(base & 0xFFFF)),
		Instruction::LEA(2, 1, static_cast<u16>(32767)),   // the largest reachable forwards
		Instruction::LEA(3, 1, static_cast<u16>(-32768)),  // and backwards
	};
	m.step(4);

	CHECK_EQ(m.reg(2), base + 32767);
	CHECK_EQ(m.reg(3), base - 32768);
}

TEST(vm, lea_computes_an_address_without_touching_memory)
{
	Machine m{
		Instruction::LI(1, 0x0400),
		Instruction::LEA(2, 1, 0x20),
	};
	m.step(2);

	CHECK_EQ(m.reg(2), 0x0420u);
}

// --- Control flow ---------------------------------------------------------------------------

TEST(vm, jump_is_relative_to_the_jump_instruction_itself)
{
	const u32 entry = Memory::UnrestrictedSegmentStart.value();

	Machine m{ Instruction::JP(i24(8)) };
	m.step();

	CHECK_EQ(m.pc().value(), entry + 8);
}

TEST(vm, a_backward_call_lands_before_the_call_site)
{
	// CALL used to widen its displacement with zeroes instead of sign, sending a backward call
	// roughly 16 MiB forward. Fixed by sign-extending like JP already did.
	const u32 entry = Memory::UnrestrictedSegmentStart.value();

	Machine m{
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::CALL(i24(-4)),
	};
	m.step(3);

	const u32 callSite = entry + 2 * Instruction::Size;
	CHECK_EQ(m.pc().value(), callSite - 4);
}

TEST(vm, call_pushes_the_return_address_and_ret_pops_it)
{
	const u32 entry = Memory::UnrestrictedSegmentStart.value();

	Machine m{
		Instruction::CALL(i24(8)),
		Instruction::HALT(),
		Instruction::RET(),
	};
	m.step();                       // CALL
	CHECK_EQ(m.pc().value(), entry + 8);

	m.step();                       // RET
	CHECK_EQ(m.pc().value(), entry + Instruction::Size);
}

TEST(vm, conditional_jump_is_taken_only_when_the_flag_is_set)
{
	const u32 entry = Memory::UnrestrictedSegmentStart.value();

	Machine taken{
		Instruction::LI(1, 5),
		Instruction::CMPI(1, 5),
		Instruction::JZ(i24(16)),
	};
	taken.step(3);
	CHECK_EQ(taken.pc().value(), entry + 2 * Instruction::Size + 16);

	Machine notTaken{
		Instruction::LI(1, 5),
		Instruction::CMPI(1, 6),
		Instruction::JZ(i24(16)),
	};
	notTaken.step(3);
	CHECK_EQ(notTaken.pc().value(), entry + 3 * Instruction::Size);
}

// --- Stack ----------------------------------------------------------------------------------

TEST(vm, push_then_pop_restores_the_value)
{
	// The factory encodes the destination in Rd; the handler used to read Rs, so every POP
	// landed in r0 regardless of what was asked for.
	Machine m{
		Instruction::LI(1, 0x2222),
		Instruction::PUSH(1),
		Instruction::POP(5),
	};
	m.step(3);

	CHECK_EQ(m.reg(5), 0x2222u);
}

// --- Conversions ------------------------------------------------------------------------------

TEST(vm, integer_to_float_and_back)
{
	Machine m{
		Instruction::LI(1, 7),
		Instruction::ITOF(0, 1),
		Instruction::FTOI(2, 0),
	};
	m.step(3);

	CHECK_EQ(m.freg(0), 7.0f);
	CHECK_EQ(m.reg(2), 7u);
}

TEST(vm, mtf_moves_the_bit_pattern_without_converting)
{
	Machine m{
		Instruction::LUI(1, 0x4048),      // 0x40480000 ~ 3.125f
		Instruction::MTF(0, 1),
	};
	m.step(2);

	CHECK_EQ(std::bit_cast<u32>(m.freg(0)), 0x40480000u);
}

// --- Halting ----------------------------------------------------------------------------------

TEST(vm, halt_sets_the_halting_flag)
{
	Machine m{ Instruction::HALT() };
	m.step();

	CHECK(m.flags().halting());
}

// --- The dispatch table ------------------------------------------------------------------------

TEST(vm, the_disassembler_names_every_mapped_opcode)
{
	// A cheap guard against an opcode being added to the enum and forgotten everywhere else.
	const Opcode mapped[] = {
		Opcode::NOP, Opcode::HALT, Opcode::ADD, Opcode::ADDI, Opcode::FADD, Opcode::SUB,
		Opcode::MUL, Opcode::DIV, Opcode::MOD, Opcode::FNEG, Opcode::AND, Opcode::OR,
		Opcode::XOR, Opcode::NOT, Opcode::SHL, Opcode::SHR, Opcode::SAR, Opcode::MOV,
		Opcode::LI, Opcode::LUI, Opcode::LDR, Opcode::LDRB, Opcode::STR, Opcode::STRB,
		Opcode::FSTR, Opcode::LEA, Opcode::JP, Opcode::JPR, Opcode::CMP, Opcode::CMPI,
		Opcode::JZ, Opcode::CALL, Opcode::RET, Opcode::PUSH, Opcode::POP, Opcode::ITOF,
		Opcode::FTOI, Opcode::MTF, Opcode::MFF, Opcode::IN, Opcode::OUT, Opcode::OUTM,
		Opcode::OUTR, Opcode::INRM, Opcode::OUTRM, Opcode::PUSHM, Opcode::POPM,
		Opcode::LDRX, Opcode::LDRBX, Opcode::LDRHX, Opcode::LDRSBX, Opcode::LDRSHX, Opcode::FLDRX,
		Opcode::STRX, Opcode::STRBX, Opcode::STRHX, Opcode::FSTRX,
	};

	for (Opcode opcode : mapped)
	{
		const std::string text = Disassembler::disassemble(Instruction::make(opcode));
		CHECK(text.find("unknown") == std::string::npos);
	}
}

// The comparison jumps read two flags each, which is the whole reason they needed opcodes of their
// own. The cases that matter are the ones where signed and unsigned disagree: -1 is below 1 as an
// i32 and above it as a u32.
namespace
{
	// Compares two full 32-bit values, then takes the jump under test. LI only carries 16 bits and
	// zero-extends, so the operands are built with LUI+ORI - which is the only way to get a value
	// like 0xFFFFFFFF, the one where signed and unsigned ordering part company.
	u32 compareAndJump(u32 a, u32 b, Instruction jump)
	{
		Machine m{
			Instruction::LUI(1, static_cast<u16>(a >> 16)),
			Instruction::ORI(1, 1, static_cast<u16>(a & 0xFFFF)),
			Instruction::LUI(2, static_cast<u16>(b >> 16)),
			Instruction::ORI(2, 2, static_cast<u16>(b & 0xFFFF)),
			Instruction::CMP(1, 2),
			jump,                            // +20, jumps 12 bytes to the "taken" arm
			Instruction::LI(3, 100),         // +24: not taken
			Instruction::JP(i24(8)),
			Instruction::LI(3, 200),         // +32: taken
		};
		m.step(7);
		return m.reg(3);
	}
}

TEST(vm, signed_and_unsigned_ordering_jumps_disagree_where_they_should)
{
	// -1 as a 16-bit immediate sign-extends to 0xFFFFFFFF: below 1 signed, above it unsigned.
	constexpr u32 minusOne = 0xFFFFFFFFu;

	CHECK_EQ(compareAndJump(minusOne, 1, Instruction::JLS(i24(12))), u32{ 200 }); // signed: -1 < 1
	CHECK_EQ(compareAndJump(minusOne, 1, Instruction::JGR(i24(12))), u32{ 100 });
	CHECK_EQ(compareAndJump(minusOne, 1, Instruction::JAB(i24(12))), u32{ 200 }); // unsigned: huge > 1
	CHECK_EQ(compareAndJump(minusOne, 1, Instruction::JBL(i24(12))), u32{ 100 });
}

TEST(vm, ordering_jumps_include_or_exclude_equality)
{
	CHECK_EQ(compareAndJump(5, 5, Instruction::JGE(i24(12))), u32{ 200 });
	CHECK_EQ(compareAndJump(5, 5, Instruction::JGR(i24(12))), u32{ 100 });
	CHECK_EQ(compareAndJump(5, 5, Instruction::JLE(i24(12))), u32{ 200 });
	CHECK_EQ(compareAndJump(5, 5, Instruction::JLS(i24(12))), u32{ 100 });
	CHECK_EQ(compareAndJump(5, 5, Instruction::JAE(i24(12))), u32{ 200 });
	CHECK_EQ(compareAndJump(5, 5, Instruction::JBE(i24(12))), u32{ 200 });
	CHECK_EQ(compareAndJump(5, 5, Instruction::JAB(i24(12))), u32{ 100 });
	CHECK_EQ(compareAndJump(5, 5, Instruction::JBL(i24(12))), u32{ 100 });
}

TEST(vm, ordering_jumps_agree_with_plain_greater_and_less)
{
	CHECK_EQ(compareAndJump(9, 4, Instruction::JGR(i24(12))), u32{ 200 });
	CHECK_EQ(compareAndJump(4, 9, Instruction::JGR(i24(12))), u32{ 100 });
	CHECK_EQ(compareAndJump(4, 9, Instruction::JLS(i24(12))), u32{ 200 });
	CHECK_EQ(compareAndJump(9, 4, Instruction::JLS(i24(12))), u32{ 100 });
}

// --- The stack stops where the program ends ------------------------------------------------

TEST(vm, a_push_below_the_stack_limit_faults_instead_of_writing)
{
	Machine m{ Instruction::PUSH(1), Instruction::PUSH(1) };

	// Somewhere above the BIOS, standing in for the end of a loaded image.
	const u32 limit = static_cast<u32>(Memory::UnrestrictedSegmentStartValue) + 0x1000;
	const Address below = Address(limit - Instruction::Size);

	m.engine().setStackLimit(limit);
	m.engine().setRegister(15, limit + 4); // room for exactly one word
	m.engine().setRegister(1, 0xABCDEF01u);
	m.memory().writeUnchecked<u32>(below, 0xDEADBEEFu); // the program's own text, in effect

	// The fault needs a handler to dispatch to, and Machine loads no BIOS.
	m.memory().writeUnchecked<u32>(
		Address(static_cast<u32>(InterruptNumber::StackOverflow) * Address::Size),
		Memory::UnrestrictedSegmentStart.value());

	m.step(); // the first push fits
	CHECK_EQ(m.reg(15), limit);
	CHECK_EQ(m.memory().readUnchecked<u32>(Address(limit)), 0xABCDEF01u);

	m.step(); // the second one has nowhere left to go
	CHECK_EQ(m.reg(15), limit); // sp did not move
	CHECK_EQ(m.memory().readUnchecked<u32>(below), 0xDEADBEEFu); // and nothing below it was touched

	// Nowhere to push the fault's own two words either, so the machine stops rather than
	// re-entering the dispatch forever.
	CHECK(m.flags().trap());
	CHECK(m.flags().halting());
}

TEST(vm, the_stack_limit_defaults_to_the_top_of_the_bios)
{
	// With no program loaded there is no image to defend, so the guard sits where it always did.
	Machine m{ Instruction::PUSH(1) };
	CHECK_EQ(m.engine().stackLimit(), static_cast<u32>(Memory::UnrestrictedSegmentStartValue));
}

// --- The program cannot write over its own text ---------------------------------------------

TEST(vm, a_store_into_the_text_range_faults_and_leaves_the_instruction_alone)
{
	Machine m{ Instruction::STR(1, 2, 0), Instruction::STRB(1, 2, 0) };

	const u32 textStart = static_cast<u32>(Memory::UnrestrictedSegmentStartValue);
	const u32 textEnd = textStart + 2 * Instruction::Size;
	const u32 original = m.memory().readUnchecked<u32>(Address(textStart));

	m.engine().setTextRange(textStart, textEnd);
	m.memory().writeUnchecked<u32>(
		Address(static_cast<u32>(InterruptNumber::MemoryFault) * Address::Size),
		Memory::UnrestrictedSegmentStart.value());

	m.engine().setRegister(1, textStart); // a pointer that has gone astray
	m.engine().setRegister(2, 0u);

	m.step();
	CHECK_EQ(m.memory().readUnchecked<u32>(Address(textStart)), original);

	// A byte store is just as capable of corrupting an instruction, and is checked too.
	m.engine().setTextRange(textStart, textEnd);
	m.engine().setProgramCounter(Address(textStart + Instruction::Size));
	m.step();
	CHECK_EQ(m.memory().readUnchecked<u32>(Address(textStart)), original);
}

TEST(vm, a_store_outside_the_text_range_still_works)
{
	Machine m{ Instruction::STR(1, 2, 0) };

	const u32 textStart = static_cast<u32>(Memory::UnrestrictedSegmentStartValue);
	const Address target = Address(textStart + 0x400);

	m.engine().setTextRange(textStart, textStart + Instruction::Size);
	m.engine().setRegister(1, target.value());
	m.engine().setRegister(2, 0x12345678u);

	m.step();
	CHECK_EQ(m.memory().readUnchecked<u32>(target), 0x12345678u);
}

TEST(vm, the_text_range_is_empty_until_a_program_is_loaded)
{
	Machine m{ Instruction::NOP() };
	CHECK_EQ(m.engine().textStart(), m.engine().textEnd());
}

// --- Saving a set of registers in one instruction --------------------------------------------

TEST(vm, pushm_and_popm_round_trip_whatever_the_mask)
{
	// r8-r11, the callee-saved four.
	constexpr u16 mask = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11);

	Machine m{
		Instruction::PUSHM(mask),
		Instruction::LI(8, 0), Instruction::LI(9, 0), Instruction::LI(10, 0), Instruction::LI(11, 0),
		Instruction::POPM(mask),
	};

	for (u8 r = 8; r <= 11; ++r)
		m.engine().setRegister(r, 0x1000u + r);

	const u32 spBefore = m.reg(15);

	m.step();
	CHECK_EQ(m.reg(15), spBefore - 4 * 4); // four words, and no more

	m.step(4);
	CHECK_EQ(m.reg(8), 0u);

	m.step();
	CHECK_EQ(m.reg(15), spBefore); // and the stack is back where it started
	for (u8 r = 8; r <= 11; ++r)
		CHECK_EQ(m.reg(r), 0x1000u + r);
}

TEST(vm, pushm_stores_in_the_order_popm_reads)
{
	// The lowest-numbered register has to end up at the lowest address, or a frame described by a
	// struct would not line up with what popm restores.
	constexpr u16 mask = (1u << 2) | (1u << 5);
	Machine m{ Instruction::PUSHM(mask) };

	m.engine().setRegister(2, 0x22222222u);
	m.engine().setRegister(5, 0x55555555u);
	m.step();

	const Address top = Address(m.reg(15));
	CHECK_EQ(m.memory().readUnchecked<u32>(top), 0x22222222u);
	CHECK_EQ(m.memory().readUnchecked<u32>(top + Address(4)), 0x55555555u);
}

TEST(vm, an_empty_mask_touches_nothing)
{
	Machine m{ Instruction::PUSHM(0), Instruction::POPM(0) };
	const u32 spBefore = m.reg(15);
	m.step(2);
	CHECK_EQ(m.reg(15), spBefore);
}

TEST(vm, pushm_saves_nothing_when_the_whole_mask_does_not_fit)
{
	constexpr u16 mask = (1u << 1) | (1u << 2) | (1u << 3);
	Machine m{ Instruction::PUSHM(mask) };

	const u32 limit = static_cast<u32>(Memory::UnrestrictedSegmentStartValue) + 0x1000;
	m.engine().setStackLimit(limit);
	m.engine().setRegister(15, limit + 8); // room for two words, not three
	m.memory().writeUnchecked<u32>(
		Address(static_cast<u32>(InterruptNumber::StackOverflow) * Address::Size),
		Memory::UnrestrictedSegmentStart.value());

	m.engine().setRegister(1, 0x11111111u);
	m.engine().setRegister(2, 0x22222222u);
	m.engine().setRegister(3, 0x33333333u);

	m.step();

	// Nothing of the mask was stored - the only two words that went down are the fault's own
	// saved PC and flags, so the top of the stack is the address of the instruction that
	// faulted rather than r1.
	CHECK_EQ(m.memory().readUnchecked<u32>(Address(m.reg(15))), Memory::UnrestrictedSegmentStart.value());
	CHECK_EQ(m.reg(15), limit); // eight bytes, which is the dispatch and not three registers
}

// --- Indexed addressing ----------------------------------------------------------------------

TEST(vm, an_indexed_load_adds_the_two_registers)
{
	Machine m{ Instruction::LDRX(1, 2, 3) };

	const Address base = Memory::UnrestrictedSegmentStart + Address(0x100);
	m.memory().writeUnchecked<u32>(base + Address(12), 0xFEEDFACEu);
	m.engine().setRegister(2, base.value());
	m.engine().setRegister(3, 12);

	m.step();
	CHECK_EQ(m.reg(1), 0xFEEDFACEu);
}

TEST(vm, an_indexed_store_puts_the_base_in_rd_and_the_value_in_rs)
{
	Machine m{ Instruction::STRX(1, 2, 3) };

	const Address base = Memory::UnrestrictedSegmentStart + Address(0x100);
	m.engine().setRegister(1, base.value());
	m.engine().setRegister(2, 0xCAFEBABEu);
	m.engine().setRegister(3, 8);

	m.step();
	CHECK_EQ(m.memory().readUnchecked<u32>(base + Address(8)), 0xCAFEBABEu);
}

TEST(vm, an_indexed_load_faults_on_a_misaligned_sum)
{
	// Neither register is misaligned on its own; their sum is. The check has to look at the
	// address the instruction actually forms.
	Machine m{ Instruction::LDRX(1, 2, 3) };

	m.memory().writeUnchecked<u32>(
		Address(static_cast<u32>(InterruptNumber::AlignmentFault) * Address::Size),
		Memory::UnrestrictedSegmentStart.value());
	m.engine().setRegister(1, 0xAAAAAAAAu);
	m.engine().setRegister(2, Memory::UnrestrictedSegmentStart.value());
	m.engine().setRegister(3, 2);

	m.step();
	CHECK_EQ(m.reg(1), 0xAAAAAAAAu); // the load never happened
}

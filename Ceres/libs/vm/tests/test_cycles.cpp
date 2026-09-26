// The CPU clock (plan/v2 SPEC 3.2): known sequences, and the cycles each one costs.
#include "framework.h"
#include <ceres/vm/ceresvm.h>
#include <ceres/core/isa/cycles.h>
#include <vector>

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
			_vm.memory().writeUnchecked<u32>(0_addr, entry.value());
			_vm.engine().reset();
		}

		// The cycles each of the next `count` instructions costs.
		std::vector<u64> costs(usize count)
		{
			std::vector<u64> out;
			for (usize i = 0; i < count; ++i)
			{
				const u64 before = _vm.engine().cycles();
				_vm.engine().step();
				out.push_back(_vm.engine().cycles() - before);
			}
			return out;
		}

		void place(Address at, std::initializer_list<Instruction> code)
		{
			usize offset = 0;
			for (Instruction instruction : code)
			{
				_vm.memory().writeUnchecked<u32>(at + Address(static_cast<u32>(offset)), instruction.raw());
				offset += Instruction::Size;
			}
		}

		void vector(InterruptNumber number, Address handler)
		{
			_vm.memory().writeUnchecked<u32>(Address(static_cast<u32>(number) * Address::Size), handler.value());
		}

		ExecutionEngine& engine() { return _vm.engine(); }
	};

	using Costs = std::vector<u64>;
}

TEST(cycles, a_fresh_machine_has_spent_none)
{
	Machine m{ Instruction::NOP() };
	CHECK_EQ(m.engine().cycles(), u64{ 0 });
}

TEST(cycles, alu_instructions_cost_one)
{
	Machine m{ Instruction::LI(1, 5), Instruction::ADD(2, 1, 1), Instruction::XOR(3, 2, 1), Instruction::SHLI(4, 3, 2),
		Instruction::CMP(1, 2), Instruction::MOV(5, 4), Instruction::CLZ(6, 5), Instruction::NOP() };
	CHECK(m.costs(8) == Costs(8, 1));
}

TEST(cycles, a_ram_access_costs_two_and_a_device_access_four)
{
	Machine m{
		Instruction::LUI(2, 1),            // r2 = 0x10000
		Instruction::STR(2, 1, 0), Instruction::LDR(3, 2, 0), Instruction::LDRB(3, 2, 1), Instruction::STRH(2, 3, 2),
		Instruction::LUI(4, 0xFF00),       // the terminal's slot: empty here, but still the device window
		Instruction::LDR(5, 4, 0), Instruction::STR(4, 5, 4),
	};
	CHECK(m.costs(8) == (Costs{ 1, 2, 2, 2, 2, 1, 4, 4 }));
}

TEST(cycles, a_conditional_jump_costs_one_more_when_taken)
{
	Machine m{ Instruction::LI(1, 0), Instruction::CMPI(1, 0),
		Instruction::JNZ(i24(8)),          // not taken
		Instruction::JZ(i24(4)),           // taken, to the next instruction
		Instruction::JP(i24(4)) };
	CHECK(m.costs(5) == (Costs{ 1, 1, 1, 2, 2 }));
}

TEST(cycles, call_and_ret_cost_three_and_push_and_pop_two)
{
	// call +12 lands on the ret three words down; the ret comes back to the push.
	Machine m{ Instruction::CALL(i24(12)), Instruction::PUSH(1), Instruction::POP(2), Instruction::RET() };
	CHECK(m.costs(4) == (Costs{ 3, 3, 2, 2 }));
}

TEST(cycles, a_register_list_costs_one_plus_two_a_register)
{
	Machine m{ Instruction::PUSHM(0x00F0), Instruction::POPM(0x00F0), Instruction::ENTER(8), Instruction::LEAVE(),
		Instruction::BL(11, 4) };
	CHECK(m.costs(5) == (Costs{ 1 + 2 * 4, 1 + 2 * 4, 3, 3, 3 }));
}

TEST(cycles, multiply_divide_and_float_cost_what_the_table_says)
{
	Machine m{ Instruction::LI(1, 7), Instruction::LI(2, 3),
		Instruction::MUL(3, 1, 2), Instruction::DIV(4, 1, 2), Instruction::MOD(5, 1, 2),
		Instruction::ITOF(1, 1), Instruction::ITOF(2, 2),
		Instruction::FADD(3, 1, 2), Instruction::FDIV(4, 1, 2), Instruction::FSQRT(5, 1), Instruction::FMA(6, 1, 2) };
	CHECK(m.costs(11) == (Costs{ 1, 1, 3, 16, 16, 3, 3, 3, 12, 12, 4 }));
}

TEST(cycles, a_block_instruction_costs_four_plus_one_per_eight_bytes)
{
	Machine m{
		Instruction::LUI(1, 2), Instruction::LUI(2, 3), Instruction::LI(3, 20),
		Instruction::MCPY(1, 2, 3),        // 20 bytes: 4 + 3
		Instruction::LI(3, 256), Instruction::LI(4, 0x55),
		Instruction::MSET(1, 4, 3),        // 256 bytes: 4 + 32
		Instruction::MCPY(1, 2, 3),        // r3 is 0 now: mset consumed it
	};
	const Costs costs = m.costs(8);
	CHECK_EQ(costs[3], u64{ 4 + 3 });
	CHECK_EQ(costs[6], u64{ 4 + 32 });
	CHECK_EQ(costs[7], u64{ 4 });          // mset left r3 at 0: nothing to move, the base alone
}

TEST(cycles, entering_an_interrupt_costs_twelve_and_iret_six)
{
	Machine m{ Instruction::STI(), Instruction::INT(40), Instruction::NOP() };
	m.place(Address(0x800), { Instruction::IRET() });
	m.vector(static_cast<InterruptNumber>(40), Address(0x800));
	CHECK(m.costs(3) == (Costs{ 1, isa::cycles::InterruptEntry, isa::cycles::IretCycles }));
}

TEST(cycles, profiling_counts_what_each_instruction_word_cost)
{
	Machine m{ Instruction::LUI(2, 1), Instruction::LDR(3, 2, 0), Instruction::MUL(4, 3, 3), Instruction::NOP() };
	const u32 start = Memory::UnrestrictedSegmentStart.value();
	m.engine().setTextRange(start, start + 4 * Instruction::Size);
	m.engine().enableProfiling();
	const Costs costs = m.costs(4);
	const auto counted = m.engine().cycleCounts();
	CHECK_EQ(counted.size(), usize{ 4 });
	CHECK(Costs(counted.begin(), counted.end()) == costs);
	CHECK(m.engine().executionCounts()[1] == 1u);
}

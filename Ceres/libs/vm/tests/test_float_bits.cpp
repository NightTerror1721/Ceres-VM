// The float bank keeps raw bits (plan/v2 SPEC 6.2): a signalling NaN and a NaN's payload go through every instruction
// that moves a value - fmov, push and pop, the multiple forms, loads and stores, an interrupt - bit for bit.
#include "framework.h"
#include <ceres/vm/ceresvm.h>
#include <array>

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
			place(Memory::UnrestrictedSegmentStart, program);
			_vm.memory().writeUnchecked<u32>(0_addr, Memory::UnrestrictedSegmentStart.value());
			_vm.engine().reset();
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

		void vector(InterruptNumber number, u32 handler)
		{
			_vm.memory().writeUnchecked<u32>(Address(static_cast<u32>(number) * Address::Size), handler);
		}

		void step(usize count = 1)
		{
			for (usize i = 0; i < count; ++i)
				_vm.engine().step();
		}

		u32 reg(usize index) const { return _vm.engine().registers().getValue(index); }
		u32 fbits(usize index) const { return _vm.engine().fregisters().getBits(index); }
		Memory& memory() { return _vm.memory(); }
		ExecutionEngine& engine() { return _vm.engine(); }
	};

	// A signalling NaN (quiet bit clear), a quiet NaN with a payload, and a negative signalling one.
	constexpr std::array<u32, 3> Patterns{ 0x7F800001u, 0x7FC12345u, 0xFF800123u };
}

TEST(float_bits, a_register_holds_the_bits_it_was_given)
{
	FloatingPointRegister reg = FloatingPointRegister::fromBits(0x7F800001u);
	CHECK_EQ(reg.bits(), 0x7F800001u);
	reg.setBits(0xFFC00000u);
	CHECK_EQ(reg.bits(), 0xFFC00000u);
	reg.set(1.5f);
	CHECK_EQ(reg.bits(), 0x3FC00000u);
	CHECK_EQ(reg.value(), 1.5f);
	// Equal means the same bits: -0 is not +0.
	CHECK(!(FloatingPointRegister(0.0f) == FloatingPointRegister(-0.0f)));
}

TEST(float_bits, nans_survive_fmov_push_pop_and_the_multiple_forms)
{
	for (const u32 bits : Patterns)
	{
		Machine m{
			Instruction::LUI(1, static_cast<u16>(bits >> 16)),
			Instruction::ORI(1, 1, static_cast<u16>(bits & 0xFFFFu)),
			Instruction::MTF(1, 1),
			Instruction::FMOV(2, 1),          // f2 = f1
			Instruction::FPUSH(2),
			Instruction::FPOP(3),             // f3 = f2
			Instruction::FPUSHM(0x0008),      // f3
			Instruction::FPOPM(0x0010),       // f4
			Instruction::MFF(5, 4),           // r5 = the bits of f4
		};
		m.step(9);
		CHECK_EQ(m.fbits(2), bits);
		CHECK_EQ(m.fbits(3), bits);
		CHECK_EQ(m.fbits(4), bits);
		CHECK_EQ(m.reg(5), bits);
	}
}

TEST(float_bits, nans_survive_every_load_and_store)
{
	const u32 buffer = 0x00100000u;
	for (const u32 bits : Patterns)
	{
		Machine m{
			Instruction::LUI(1, static_cast<u16>(bits >> 16)),
			Instruction::ORI(1, 1, static_cast<u16>(bits & 0xFFFFu)),
			Instruction::MTF(1, 1),
			Instruction::LUI(2, static_cast<u16>(buffer >> 16)),
			Instruction::LI(3, 8),
			Instruction::FSTR(2, 1, 0),       // [buffer] = f1
			Instruction::FLDR(4, 2, 0),       // f4 = [buffer]
			Instruction::FSTRX(2, 4, 3),      // [buffer + 8] = f4
			Instruction::FLDRX(5, 2, 3),      // f5 = [buffer + 8]
		};
		m.step(9);
		CHECK_EQ(m.memory().readUnchecked<u32>(Address(buffer)), bits);
		CHECK_EQ(m.memory().readUnchecked<u32>(Address(buffer + 8)), bits);
		CHECK_EQ(m.fbits(4), bits);
		CHECK_EQ(m.fbits(5), bits);
	}
}

TEST(float_bits, nans_survive_an_interrupt)
{
	// The handler saves and restores f1 as a handler that uses the float bank would; the interrupted code keeps its NaN.
	const u32 handler = 0x2000u;
	for (const u32 bits : Patterns)
	{
		Machine m{
			Instruction::LUI(1, static_cast<u16>(bits >> 16)),
			Instruction::ORI(1, 1, static_cast<u16>(bits & 0xFFFFu)),
			Instruction::MTF(1, 1),
			Instruction::INT(40),
			Instruction::MFF(6, 1),
		};
		m.vector(static_cast<InterruptNumber>(40), handler);
		m.place(Address(handler), { Instruction::FPUSH(1), Instruction::LI(7, 3), Instruction::ITOF(1, 7), Instruction::FPOP(1), Instruction::IRET() });
		m.step(3);
		m.engine().setFlags(FlagRegister(m.engine().flags().value() | static_cast<FlagRegister::ValueType>(ExecutionFlag::Interrupt)));
		m.step(1 + 5 + 1);
		CHECK_EQ(m.fbits(1), bits);
		CHECK_EQ(m.reg(6), bits);
	}
}

TEST(float_bits, arithmetic_still_works_on_the_value)
{
	Machine m{ Instruction::LI(1, 3), Instruction::ITOF(1, 1), Instruction::LI(2, 4), Instruction::ITOF(2, 2), Instruction::FMUL(3, 1, 2) };
	m.step(5);
	CHECK_EQ(m.engine().fregisters().getValue(3), 12.0f);
	CHECK_EQ(m.fbits(3), 0x41400000u);
}

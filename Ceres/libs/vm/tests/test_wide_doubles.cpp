// The binary64 instructions beyond the vectors (plan/v2 SPEC 6): flags, the zero divisor, one rounding in fma.d, the
// unary operations, fclass.d, the saturating conversions, memory, the moves between banks, and cycles.
#include "framework.h"
#include <ceres/vm/ceresvm.h>
#include <bit>
#include <cmath>
#include <limits>

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

		void step(usize count = 1)
		{
			for (usize i = 0; i < count; ++i)
				_vm.engine().step();
		}
		void setD(u8 pair, f64 value)
		{
			const u64 bits = std::bit_cast<u64>(value);
			_vm.engine().setFloatRegisterBits(static_cast<u8>(pair * 2), static_cast<u32>(bits));
			_vm.engine().setFloatRegisterBits(static_cast<u8>(pair * 2 + 1), static_cast<u32>(bits >> 32));
		}
		f64 d(u8 pair) const
		{
			const auto& f = _vm.engine().fregisters();
			return std::bit_cast<f64>(static_cast<u64>(f.getBits(pair * 2u)) | (static_cast<u64>(f.getBits(pair * 2u + 1u)) << 32));
		}
		void setX(u8 pair, u64 value)
		{
			_vm.engine().setRegister(static_cast<u8>(pair * 2), static_cast<u32>(value));
			_vm.engine().setRegister(static_cast<u8>(pair * 2 + 1), static_cast<u32>(value >> 32));
		}
		u64 x(u8 pair) const
		{
			const auto& r = _vm.engine().registers();
			return static_cast<u64>(r.getValue(pair * 2u)) | (static_cast<u64>(r.getValue(pair * 2u + 1u)) << 32);
		}
		void setReg(u8 index, u32 value) { _vm.engine().setRegister(index, value); }
		u32 reg(u8 index) const { return _vm.engine().registers().getValue(index); }
		const FlagRegister& flags() const { return _vm.engine().flags(); }
		Memory& memory() { return _vm.memory(); }
		ExecutionEngine& engine() { return _vm.engine(); }
	};

	u8 kind(wide::FcvtKind k) { return static_cast<u8>(k); }
	constexpr f64 Inf = std::numeric_limits<f64>::infinity();
	constexpr f64 NaN = std::numeric_limits<f64>::quiet_NaN();
}

TEST(wide_doubles, arithmetic_sets_the_flags_fadd_does)
{
	Machine m{ Instruction::FADDD(0, 1, 2) };
	m.setD(1, 1.5);
	m.setD(2, -1.5);
	m.step();
	CHECK_EQ(m.d(0), 0.0);
	CHECK(m.flags().zero() && !m.flags().sign() && !m.flags().overflow());

	Machine big{ Instruction::FMULD(0, 1, 2) };
	big.setD(1, 1e300);
	big.setD(2, -1e300);
	big.step();
	CHECK_EQ(big.d(0), -Inf);
	CHECK(big.flags().sign() && big.flags().overflow());   // finite operands, an infinite result
}

TEST(wide_doubles, a_zero_divisor_traps_unless_ieee_divide)
{
	Machine m{ Instruction::FDIVD(0, 1, 2) };
	m.setD(0, 7.0);
	m.setD(1, 1.0);
	m.setD(2, 0.0);
	m.step();
	CHECK(m.flags().trap());
	CHECK_EQ(m.d(0), 7.0);                                  // left alone

	Machine ieee{ Instruction::FDIVD(0, 1, 2), Instruction::FMODD(3, 1, 2) };
	ieee.engine().setIeeeDivide(true);
	ieee.setD(1, -1.0);
	ieee.setD(2, 0.0);
	ieee.step(2);
	CHECK_EQ(ieee.d(0), -Inf);
	CHECK(std::isnan(ieee.d(3)));
	CHECK(!ieee.flags().trap());

	Machine mod{ Instruction::FMODD(0, 1, 2) };
	mod.setD(0, 7.0);
	mod.setD(1, 1.0);
	mod.setD(2, 0.0);
	mod.step();
	CHECK(mod.flags().trap());
	CHECK_EQ(mod.d(0), 7.0);
}

TEST(wide_doubles, fma_d_rounds_once)
{
	// (1 + 2^-52)(1 - 2^-52) - 1 = -2^-104 exactly; rounding the product first would give 0.
	Machine m{ Instruction::FMAD(0, 1, 2) };
	m.setD(0, -1.0);
	m.setD(1, 1.0 + 0x1p-52);
	m.setD(2, 1.0 - 0x1p-52);
	m.step();
	CHECK_EQ(m.d(0), -0x1p-104);
}

TEST(wide_doubles, the_unary_operations_min_max_copysign_and_fclass)
{
	const auto unary = [](Instruction instruction, f64 value)
	{
		Machine m{ instruction };
		m.setD(1, value);
		m.step();
		return m.d(0);
	};
	CHECK_EQ(unary(Instruction::FNEGD(0, 1), 2.5), -2.5);
	CHECK_EQ(unary(Instruction::FABSD(0, 1), -2.5), 2.5);
	CHECK_EQ(unary(Instruction::FROUNDD(0, 1), 2.5), 2.0);   // ties to even
	CHECK_EQ(unary(Instruction::FROUNDD(0, 1), 3.5), 4.0);
	CHECK_EQ(unary(Instruction::FFLOORD(0, 1), -2.5), -3.0);
	CHECK_EQ(unary(Instruction::FCEILD(0, 1), -2.5), -2.0);
	CHECK_EQ(unary(Instruction::FTRUNCD(0, 1), -2.5), -2.0);
	CHECK_EQ(unary(Instruction::FSQRTD(0, 1), 2.25), 1.5);

	Machine minmax{ Instruction::FMIND(0, 1, 2), Instruction::FMAXD(3, 1, 2), Instruction::FCOPYSIGND(4, 1, 2) };
	minmax.setD(1, 3.0);
	minmax.setD(2, -0.5);
	minmax.step(3);
	CHECK_EQ(minmax.d(0), -0.5);
	CHECK_EQ(minmax.d(3), 3.0);
	CHECK_EQ(minmax.d(4), -3.0);

	const auto classify = [](f64 value)
	{
		Machine m{ Instruction::FCLASSD(5, 1) };
		m.setD(1, value);
		m.step();
		return m.reg(5);
	};
	CHECK_EQ(classify(-Inf), 1u << 0);
	CHECK_EQ(classify(-1.0), 1u << 1);
	CHECK_EQ(classify(-5e-324), 1u << 2);
	CHECK_EQ(classify(-0.0), 1u << 3);
	CHECK_EQ(classify(0.0), 1u << 4);
	CHECK_EQ(classify(5e-324), 1u << 5);
	CHECK_EQ(classify(1.0), 1u << 6);
	CHECK_EQ(classify(Inf), 1u << 7);
	CHECK_EQ(classify(NaN), 1u << 8);
}

TEST(wide_doubles, conversions_to_integers_truncate_and_saturate)
{
	const auto toWord = [](wide::FcvtKind k, f64 value)
	{
		Machine m{ Instruction::FCVT(kind(k), 5, 1) };
		m.setD(1, value);
		m.step();
		return m.reg(5);
	};
	CHECK_EQ(toWord(wide::FcvtKind::WD, -2.9), static_cast<u32>(-2));
	CHECK_EQ(toWord(wide::FcvtKind::WD, 1e20), 0x7FFFFFFFu);
	CHECK_EQ(toWord(wide::FcvtKind::WD, -1e20), 0x80000000u);
	CHECK_EQ(toWord(wide::FcvtKind::WD, NaN), 0u);
	CHECK_EQ(toWord(wide::FcvtKind::WUD, -3.0), 0u);
	CHECK_EQ(toWord(wide::FcvtKind::WUD, 4294967295.9), 0xFFFFFFFFu);
	CHECK_EQ(toWord(wide::FcvtKind::WUD, 1e20), 0xFFFFFFFFu);

	const auto fromFloat = [](wide::FcvtKind k, f32 value)
	{
		Machine m{ Instruction::FCVT(kind(k), 1, 3) };
		m.engine().setFloatRegister(3, value);
		m.step();
		return m.x(1);
	};
	CHECK_EQ(fromFloat(wide::FcvtKind::LS, -3.75f), static_cast<u64>(i64{ -3 }));
	CHECK_EQ(fromFloat(wide::FcvtKind::LS, 1e30f), static_cast<u64>(std::numeric_limits<i64>::max()));
	CHECK_EQ(fromFloat(wide::FcvtKind::LUS, -1.0f), u64{ 0 });

	// And to floats, from 32 and 64-bit integers.
	Machine m{ Instruction::FCVT(kind(wide::FcvtKind::DW), 0, 5), Instruction::FCVT(kind(wide::FcvtKind::DWU), 1, 5),
		Instruction::FCVT(kind(wide::FcvtKind::SL), 4, 3), Instruction::FCVT(kind(wide::FcvtKind::SLU), 5, 3) };
	m.setReg(5, 0xFFFFFFFFu);
	m.setX(3, ~u64{ 0 });
	m.step(4);
	CHECK_EQ(m.d(0), -1.0);
	CHECK_EQ(m.d(1), 4294967295.0);
	CHECK_EQ(m.engine().fregisters().getValue(4), -1.0f);
	CHECK_EQ(m.engine().fregisters().getValue(5), 18446744073709551616.0f);
}

TEST(wide_doubles, memory_and_the_moves_between_banks_keep_the_bits)
{
	const u32 buffer = 0x00100004u;
	Machine m{
		Instruction::MTFD(1, 2),          // d1 = the bits of x2
		Instruction::FSTRD(8, 1, 0),      // [r8] = d1
		Instruction::FLDRD(2, 8, 0),      // d2 = [r8]
		Instruction::FSTRDX(8, 2, 9),     // [r8 + r9] = d2
		Instruction::FLDRDX(3, 8, 9),     // d3 = [r8 + r9]
		Instruction::FMOVD(4, 3),
		Instruction::MFFD(3, 4),          // x3 = the bits of d4
	};
	const u64 signalling = 0x7FF0000000000001u;   // a signalling NaN: bits, not a value
	m.setX(2, signalling);
	m.setReg(8, buffer);
	m.setReg(9, 8);
	m.step(7);
	CHECK_EQ(m.memory().readUnchecked<u32>(Address(buffer)), 0x00000001u);
	CHECK_EQ(m.memory().readUnchecked<u32>(Address(buffer + 4)), 0x7FF00000u);
	CHECK_EQ(m.x(3), signalling);

	Machine pc{ Instruction::FLDRDP(1, 8), Instruction::HALT(), Instruction(0x00000000u), Instruction(0x3FF80000u) };
	pc.step();
	CHECK_EQ(pc.d(1), 1.5);
}

TEST(wide_doubles, cycles_as_spec_6_4_has_them)
{
	const auto cost = [](Instruction instruction)
	{
		Machine m{ instruction };
		m.setReg(8, 0x00100000u);
		m.setD(2, 1.0);
		const u64 before = m.engine().cycles();
		m.step();
		return m.engine().cycles() - before;
	};
	CHECK_EQ(cost(Instruction::FADDD(0, 1, 2)), u64{ 4 });
	CHECK_EQ(cost(Instruction::FMULD(0, 1, 2)), u64{ 5 });
	CHECK_EQ(cost(Instruction::FDIVD(0, 1, 2)), u64{ 20 });
	CHECK_EQ(cost(Instruction::FMAD(0, 1, 2)), u64{ 6 });
	CHECK_EQ(cost(Instruction::FSQRTD(0, 1)), u64{ 24 });
	CHECK_EQ(cost(Instruction::FMODD(0, 1, 2)), u64{ 30 });
	CHECK_EQ(cost(Instruction::FCMPD(1, 2)), u64{ 3 });
	CHECK_EQ(cost(Instruction::FCOPYSIGND(0, 1, 2)), u64{ 2 });
	CHECK_EQ(cost(Instruction::FCVT(kind(wide::FcvtKind::SD), 0, 1)), u64{ 3 });
	CHECK_EQ(cost(Instruction::FCVT(kind(wide::FcvtKind::DL), 0, 1)), u64{ 4 });
	CHECK_EQ(cost(Instruction::FMOVD(0, 1)), u64{ 1 });
	CHECK_EQ(cost(Instruction::FLDRD(1, 8, 0)), u64{ 3 });
	CHECK_EQ(cost(Instruction::FSTRD(8, 1, 0)), u64{ 3 });
}

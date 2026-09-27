// The binary64 instructions against the STDLIB's vectors (plan/v2 F3.5): operands and results computed by node, which
// is IEEE binary64 rounded to nearest even on the same kind of host. Every result must match bit for bit, NaNs and
// subnormals included. The vectors are a copy of the STDLIB's tests/f64_vectors.inc (tools/gen_f64_vectors.js there).
#include "framework.h"
#include <ceres/vm/ceresvm.h>
#include <format>
#include <string>

#include "../../../tests/data/f64_vectors.inc"

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::testing;

namespace
{
	// One instruction, run again and again with new operands: d1 and d2 (or x1, f2, r2) in, d0 (or x0, f0, r0) out.
	class Machine
	{
	private:
		CeresVM _vm;

	public:
		explicit Machine(Instruction instruction)
		{
			const Address entry = Memory::UnrestrictedSegmentStart;
			_vm.memory().writeUnchecked<u32>(entry, instruction.raw());
			_vm.memory().writeUnchecked<u32>(0_addr, entry.value());
			_vm.engine().reset();
			_vm.engine().setIeeeDivide(true);   // node divides by zero the IEEE way
		}

		void run()
		{
			_vm.engine().setProgramCounter(Memory::UnrestrictedSegmentStart);
			_vm.engine().step();
		}

		void setDouble(u8 pair, u64 bits)
		{
			_vm.engine().setFloatRegisterBits(static_cast<u8>(pair * 2), static_cast<u32>(bits));
			_vm.engine().setFloatRegisterBits(static_cast<u8>(pair * 2 + 1), static_cast<u32>(bits >> 32));
		}
		u64 getDouble(u8 pair) const
		{
			const auto& f = _vm.engine().fregisters();
			return static_cast<u64>(f.getBits(pair * 2u)) | (static_cast<u64>(f.getBits(pair * 2u + 1u)) << 32);
		}
		void setPair(u8 pair, u64 value)
		{
			_vm.engine().setRegister(static_cast<u8>(pair * 2), static_cast<u32>(value));
			_vm.engine().setRegister(static_cast<u8>(pair * 2 + 1), static_cast<u32>(value >> 32));
		}
		u64 getPair(u8 pair) const
		{
			const auto& r = _vm.engine().registers();
			return static_cast<u64>(r.getValue(pair * 2u)) | (static_cast<u64>(r.getValue(pair * 2u + 1u)) << 32);
		}
		void setFloatBits(u8 index, u32 bits) { _vm.engine().setFloatRegisterBits(index, bits); }
		u32 floatBits(u8 index) const { return _vm.engine().fregisters().getBits(index); }
		const FlagRegister& flags() const { return _vm.engine().flags(); }
	};

	// Counts the mismatches of one table and reports the first few.
	class Mismatches
	{
	private:
		std::string _table;
		usize _count = 0;

	public:
		explicit Mismatches(std::string table) : _table(std::move(table)) {}
		~Mismatches() { CHECK_EQ(_count, usize{ 0 }); }

		void compare(usize row, u64 got, u64 want)
		{
			if (got == want)
				return;
			if (++_count <= 5)
				Registry::instance().recordFailure(std::format("{} row {}: got {:#018x}, want {:#018x}", _table, row, got, want));
		}
	};

	void binary(Instruction instruction, const unsigned long long (*table)[3], usize count, const char* name)
	{
		Machine m{ instruction };
		Mismatches mismatches{ name };
		for (usize i = 0; i < count; ++i)
		{
			m.setDouble(1, table[i][0]);
			m.setDouble(2, table[i][1]);
			m.run();
			mismatches.compare(i, m.getDouble(0), table[i][2]);
		}
	}
}

TEST(f64_vectors, add_sub_mul_div_match_bit_for_bit)
{
	binary(Instruction::FADDD(0, 1, 2), v_add, V_ADD_COUNT, "v_add");
	binary(Instruction::FSUBD(0, 1, 2), v_sub, V_SUB_COUNT, "v_sub");
	binary(Instruction::FMULD(0, 1, 2), v_mul, V_MUL_COUNT, "v_mul");
	binary(Instruction::FDIVD(0, 1, 2), v_div, V_DIV_COUNT, "v_div");
}

TEST(f64_vectors, sqrt_matches_bit_for_bit)
{
	Machine m{ Instruction::FSQRTD(0, 1) };
	Mismatches mismatches{ "v_sqrt" };
	for (usize i = 0; i < V_SQRT_COUNT; ++i)
	{
		m.setDouble(1, v_sqrt[i][0]);
		m.run();
		mismatches.compare(i, m.getDouble(0), v_sqrt[i][1]);
	}
}

TEST(f64_vectors, fcmp_d_orders_as_node_does)
{
	// -1 less, 0 equal, 1 greater, 2 unordered: Zero says equal and Carry says less, as fcmp's flags do.
	Machine m{ Instruction::FCMPD(1, 2) };
	Mismatches mismatches{ "v_cmp" };
	for (usize i = 0; i < V_CMP_COUNT; ++i)
	{
		m.setDouble(1, v_cmp[i][0]);
		m.setDouble(2, v_cmp[i][1]);
		m.run();
		const i64 order = static_cast<i64>(v_cmp[i][2]);
		const u64 got = m.flags().zero() ? 0 : m.flags().carry() ? static_cast<u64>(i64{ -1 }) : 1;
		const u64 want = order == 2 ? 1 : static_cast<u64>(order);   // unordered reads as neither equal nor less
		mismatches.compare(i, got, want);
	}
}

TEST(f64_vectors, the_float_conversions_match_bit_for_bit)
{
	Machine narrow{ Instruction::FCVT(static_cast<u8>(wide::FcvtKind::SD), 0, 1) };
	Mismatches toFloat{ "v_to_f32" };
	for (usize i = 0; i < V_TO_F32_COUNT; ++i)
	{
		narrow.setDouble(1, v_to_f32[i][0]);
		narrow.run();
		toFloat.compare(i, narrow.floatBits(0), v_to_f32[i][1]);
	}

	Machine widen{ Instruction::FCVT(static_cast<u8>(wide::FcvtKind::DS), 0, 2) };
	Mismatches fromFloat{ "v_from_f32" };
	for (usize i = 0; i < V_FROM_F32_COUNT; ++i)
	{
		widen.setFloatBits(2, static_cast<u32>(v_from_f32[i][0]));
		widen.run();
		fromFloat.compare(i, widen.getDouble(0), v_from_f32[i][1]);
	}
}

TEST(f64_vectors, the_64_bit_integer_conversions_match_bit_for_bit)
{
	Machine fromSigned{ Instruction::FCVT(static_cast<u8>(wide::FcvtKind::DL), 0, 1) };
	Machine fromUnsigned{ Instruction::FCVT(static_cast<u8>(wide::FcvtKind::DLU), 0, 1) };
	Mismatches signedFrom{ "v_from_int (signed)" };
	Mismatches unsignedFrom{ "v_from_int (unsigned)" };
	for (usize i = 0; i < V_FROM_INT_COUNT; ++i)
	{
		fromSigned.setPair(1, v_from_int[i][0]);
		fromSigned.run();
		signedFrom.compare(i, fromSigned.getDouble(0), v_from_int[i][1]);
		fromUnsigned.setPair(1, v_from_int[i][0]);
		fromUnsigned.run();
		unsignedFrom.compare(i, fromUnsigned.getDouble(0), v_from_int[i][2]);
	}

	// Truncated toward zero and saturated; NaN is 0 (SPEC 6.5).
	Machine toSigned{ Instruction::FCVT(static_cast<u8>(wide::FcvtKind::LD), 0, 1) };
	Machine toUnsigned{ Instruction::FCVT(static_cast<u8>(wide::FcvtKind::LUD), 0, 1) };
	Mismatches signedTo{ "v_to_int (signed)" };
	Mismatches unsignedTo{ "v_to_int (unsigned)" };
	for (usize i = 0; i < V_TO_INT_COUNT; ++i)
	{
		toSigned.setDouble(1, v_to_int[i][0]);
		toSigned.run();
		signedTo.compare(i, toSigned.getPair(0), v_to_int[i][1]);
		toUnsigned.setDouble(1, v_to_int[i][0]);
		toUnsigned.run();
		unsignedTo.compare(i, toUnsigned.getPair(0), v_to_int[i][2]);
	}
}

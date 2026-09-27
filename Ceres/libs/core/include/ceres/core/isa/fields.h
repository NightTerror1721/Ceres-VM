#pragma once

#include <ceres/core/base/types.h>

// Where each field of an instruction word sits: one table, and every accessor of Instruction built on it.
// Shifts and masks on the u32 value, never a union or bitfields: the value is the same number on every host,
// and a bitfield's bit order is up to the compiler (plan/v2 SPEC 6.6, D13).
namespace ceres::isa
{
	template <u32 Pos, u32 Width>
	struct Field
	{
		static_assert(Width > 0 && Pos + Width <= 32, "a field lies inside the 32-bit word");

		static inline constexpr u32 Shift = Pos;
		static inline constexpr u32 Bits = Width;
		static inline constexpr u32 Mask = (Width == 32 ? ~0u : ((1u << Width) - 1u)) << Pos;

		static constexpr u32 get(u32 raw) noexcept { return (raw & Mask) >> Pos; }
		static constexpr u32 set(u32 raw, u32 value) noexcept { return (raw & ~Mask) | ((value << Pos) & Mask); }
	};

	namespace fields
	{
		using Opcode = Field<24, 8>;
		using Rd = Field<20, 4>;        // also Fd: the float registers use the same positions
		using Rs = Field<16, 4>;
		using Rt = Field<12, 4>;
		using Imm8 = Field<0, 8>;
		using Imm16 = Field<0, 16>;
		using Imm20 = Field<0, 20>;     // BL's displacement, once Rd has taken 23:20
		using Imm24 = Field<0, 24>;

		// The subfields of the 64-bit instructions (plan/v2 SPEC 6.4), in the bits the registers leave: 11:0 after three,
		// 7:0 after two.
		using ShiftKind = Field<6, 2>;  // SHI64: 0 shl64, 1 shr64, 2 sar64 (3 is not an instruction)
		using ShiftAmount = Field<0, 6>; // SHI64: 0-63
		using BitOp = Field<0, 2>;      // BITS64: 0 clz64, 1 ctz64, 2 popcnt64 (3 is not an instruction)
		using MinMax = Field<0, 1>;     // FMINMAXD: 0 fmin.d, 1 fmax.d
		using UnaryOp = Field<0, 3>;    // FUNARYD: 0 fneg.d … 5 ftrunc.d (6 and 7 are not instructions)
		using FcvtKind = Field<0, 4>;   // FCVT: SPEC 6.5 (14 and 15 are not instructions)
	}
}

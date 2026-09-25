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
	}
}

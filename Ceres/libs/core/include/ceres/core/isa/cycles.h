#pragma once

#include "opcodes.h"
#include <array>

// What each instruction costs the CPU's clock, in cycles (plan/v2 SPEC 3.2, normative since F0.6). The table holds
// the part every execution of an opcode pays; the rest is added where it happens:
//
// - every load or store adds the cost of what it reached (RAM, VRAM, a device): a stack push is a store, so push is
//   0 here and 2 in total, call is 1 + a push, pushm of n registers 1 + n stores;
// - a conditional jump adds TakenBranchExtra when it is taken;
// - a block instruction adds ceil(bytes / BlockBytesPerCycle) for each chunk it moves, and BlockBase when it ends;
// - entering an interrupt costs InterruptEntry in all, and iret IretCycles, whatever they push and pop.
namespace ceres::isa::cycles
{
	inline constexpr u32 RamAccess = 2;
	inline constexpr u32 VramAccess = 3;
	inline constexpr u32 MmioAccess = 4;
	inline constexpr u32 TakenBranchExtra = 1;
	inline constexpr u32 BlockBase = 4;
	inline constexpr u32 BlockBytesPerCycle = 8;
	inline constexpr u32 InterruptEntry = 12;
	inline constexpr u32 IretCycles = 6;

	constexpr u32 ofBlockChunk(u32 bytes) noexcept { return (bytes + BlockBytesPerCycle - 1) / BlockBytesPerCycle; }

	namespace detail
	{
		constexpr std::array<u8, 256> build() noexcept
		{
			std::array<u8, 256> table{};
			table.fill(1);   // ALU, logic, shifts, moves, compares, bit operations, system instructions
			const auto set = [&table](u8 cost, std::initializer_list<Opcode> opcodes) {
				for (const Opcode opcode : opcodes)
					table[static_cast<u8>(opcode)] = cost;
			};
			using enum Opcode;
			// The access pays: 2 for RAM, 4 for a device.
			set(0, { LDR, LDRB, LDRH, LDRSB, LDRSH, FLDR, STR, STRB, STRH, FSTR, LDRX, LDRBX, LDRHX, LDRSBX, LDRSHX, FLDRX,
				STRX, STRBX, STRHX, FSTRX, LDRP, LDRBP, LDRHP, LDRSBP, LDRSHP, FLDRP, STRP, STRBP, STRHP, FSTRP,
				PUSH, POP, PUSHF, POPF, FPUSH, FPOP });
			// 1 + the stack access: 3 in all; pushm/popm 1 + 2 per register.
			set(1, { CALL, CALLR, RET, ENTER, LEAVE, PUSHM, POPM, FPUSHM, FPOPM });
			set(3, { BL, BLR });
			set(2, { JP, JPR });   // conditional jumps stay at 1, plus TakenBranchExtra when taken
			set(3, { MUL, MULI, IMUL, IMULI, MULH, IMULH });
			set(16, { DIV, DIVI, IDIV, IDIVI, MOD, MODI, IMOD, IMODI });
			set(3, { FADD, FSUB, FMUL, FCMP, FMIN, FMAX, FABS, FNEG, ITOF, IITOF, FTOI, FTOII, FROUND, FFLOOR, FCEIL, FTRUNC,
				FCOPYSIGN, FCLASS });
			set(12, { FDIV, FSQRT, FMOD, FRECIPE, FRSQRTE });
			set(4, { FMA });
			// The 64-bit integers (plan/v2 SPEC 6.4). A 64-bit load or store is 1 + its access: 3 in RAM, where the two words
			// are one access, and 1 + 4 + 4 on a device, where they are two.
			set(2, { ADD64, SUB64, NEG64, CMP64, SHL64, SHR64, SAR64, SHI64, BITS64 });
			set(4, { MULL, IMULL });
			set(6, { MUL64 });
			set(40, { DIV64, IDIV64, MOD64, IMOD64 });
			set(1, { SXT64, MOV64, LDRD, STRD, LDRDX, STRDX, LDRDP });
			// The doubles; fcvt is 3, and one more when it reads or writes a 64-bit integer (the handler adds it).
			set(4, { FADDD, FSUBD });
			set(5, { FMULD });
			set(20, { FDIVD });
			set(6, { FMAD });
			set(24, { FSQRTD });
			set(3, { FCMPD, FMINMAXD, FUNARYD, FCVT });
			set(30, { FMODD });
			set(2, { FCOPYSIGND, FCLASSD });
			set(1, { FMOVD, MTFD, MFFD, FLDRD, FSTRD, FLDRDX, FSTRDX, FLDRDP });
			// Paid per chunk and at the end; an interrupt's entry and iret set their own totals.
			set(0, { MCPY, MSET, MCMP, MSCAN, INT, TRAP, IRET });
			return table;
		}
	}

	// The fixed part of each opcode's cost; an opcode the machine does not have costs 1 (and then faults).
	inline constexpr std::array<u8, 256> Base = detail::build();
}

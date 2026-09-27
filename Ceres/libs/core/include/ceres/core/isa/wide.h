#pragma once

#include "instructions.h"
#include <array>
#include <string_view>

// The 64-bit instructions (plan/v2 SPEC 6): which bank each register field names, what the subfields mean and
// whether a word is one the machine runs. One table for the VM's decoding faults, the disassembler, the assembler
// and the tests, so the four cannot disagree.
namespace ceres::isa::wide
{
	// What a register field names. A pair field holds its even register's number: x3 is 6 (r6:r7), d3 is 6 (f6:f7).
	enum class Bank : u8
	{
		None,
		Int,         // rN
		Float,       // fN
		IntPair,     // xN, N = 0..6: x7 would be r14:r15, fp:sp
		DoublePair,  // dN, N = 0..7
	};

	// How the operands are written, in the order assembly writes them.
	enum class Form : u8
	{
		Three,       // add64 x1, x2, x3
		Two,         // neg64 x1, x2
		Compare,     // cmp64 x1, x2 (rs, rt)
		ShiftImm,    // shl64 x1, x2, 12
		Load,        // ldrd x1, [r2 + 8]
		Store,       // strd [r2 + 8], x1 (base in rd, pair in rs)
		LoadIndexed, // ldrd x1, [r2 + r3]
		StoreIndexed,// strd [r2 + r3], x1 (base in rd, index in rt, pair in rs)
		LoadPc,      // ldrd x1, [pc + 8]
	};

	enum class ShiftKind : u8 { Shl = 0, Shr = 1, Sar = 2 };
	enum class BitOp : u8 { Clz = 0, Ctz = 1, Popcnt = 2 };
	enum class UnaryOp : u8 { Neg = 0, Abs = 1, Round = 2, Floor = 3, Ceil = 4, Trunc = 5 };

	// SPEC 6.5: w a 32-bit signed integer, wu unsigned, l a signed integer pair, lu unsigned, s a float, d a double pair.
	enum class FcvtKind : u8
	{
		DS = 0, SD = 1, DW = 2, DWU = 3, WD = 4, WUD = 5, DL = 6, DLU = 7,
		LD = 8, LUD = 9, SL = 10, SLU = 11, LS = 12, LUS = 13,
	};
	inline constexpr u32 FcvtKindCount = 14;

	struct Conversion
	{
		std::string_view name;
		Bank to;      // rd
		Bank from;    // rs
	};

	inline constexpr std::array<Conversion, FcvtKindCount> Conversions{ {
		{ "fcvt.d.s", Bank::DoublePair, Bank::Float },
		{ "fcvt.s.d", Bank::Float, Bank::DoublePair },
		{ "fcvt.d.w", Bank::DoublePair, Bank::Int },
		{ "fcvt.d.wu", Bank::DoublePair, Bank::Int },
		{ "fcvt.w.d", Bank::Int, Bank::DoublePair },
		{ "fcvt.wu.d", Bank::Int, Bank::DoublePair },
		{ "fcvt.d.l", Bank::DoublePair, Bank::IntPair },
		{ "fcvt.d.lu", Bank::DoublePair, Bank::IntPair },
		{ "fcvt.l.d", Bank::IntPair, Bank::DoublePair },
		{ "fcvt.lu.d", Bank::IntPair, Bank::DoublePair },
		{ "fcvt.s.l", Bank::Float, Bank::IntPair },
		{ "fcvt.s.lu", Bank::Float, Bank::IntPair },
		{ "fcvt.l.s", Bank::IntPair, Bank::Float },
		{ "fcvt.lu.s", Bank::IntPair, Bank::Float },
	} };

	inline constexpr std::array<std::string_view, 3> ShiftNames{ "shl64", "shr64", "sar64" };
	inline constexpr std::array<std::string_view, 3> BitOpNames{ "clz64", "ctz64", "popcnt64" };
	inline constexpr std::array<std::string_view, 6> UnaryNames{ "fneg.d", "fabs.d", "fround.d", "ffloor.d", "fceil.d", "ftrunc.d" };

	// One 64-bit instruction as the machine decodes it: its name, how it is written and the bank of each field.
	struct Entry
	{
		std::string_view name;
		Form form = Form::Three;
		Bank rd = Bank::None;
		Bank rs = Bank::None;
		Bank rt = Bank::None;
	};

	// Whether an opcode is one of the 64-bit instructions.
	constexpr bool isWide(Opcode opcode) noexcept
	{
		const u8 value = static_cast<u8>(opcode);
		return (value >= 0xD7 && value <= 0xFE && value != 0xEE) || (value >= 0x7A && value <= 0x7E);
	}

	// Why a word is not an instruction the machine runs; Ok when it is one (or is not a 64-bit instruction at all).
	enum class Decode : u8 { Ok, RegisterPair, BadSubfield };

	namespace detail
	{
		constexpr Entry fixed(Opcode opcode) noexcept
		{
			using enum Opcode;
			constexpr Bank X = Bank::IntPair, D = Bank::DoublePair, R = Bank::Int, N = Bank::None;
			switch (opcode)
			{
				case ADD64:      return { "add64", Form::Three, X, X, X };
				case SUB64:      return { "sub64", Form::Three, X, X, X };
				case NEG64:      return { "neg64", Form::Two, X, X, N };
				case CMP64:      return { "cmp64", Form::Compare, N, X, X };
				case MULL:       return { "mull", Form::Three, X, R, R };
				case IMULL:      return { "imull", Form::Three, X, R, R };
				case MUL64:      return { "mul64", Form::Three, X, X, X };
				case DIV64:      return { "div64", Form::Three, X, X, X };
				case IDIV64:     return { "idiv64", Form::Three, X, X, X };
				case MOD64:      return { "mod64", Form::Three, X, X, X };
				case IMOD64:     return { "imod64", Form::Three, X, X, X };
				case SHL64:      return { "shl64", Form::Three, X, X, R };
				case SHR64:      return { "shr64", Form::Three, X, X, R };
				case SAR64:      return { "sar64", Form::Three, X, X, R };
				case SHI64:      return { "", Form::ShiftImm, X, X, N };
				case SXT64:      return { "sxt64", Form::Two, X, R, N };
				case BITS64:     return { "", Form::Two, R, X, N };
				case MOV64:      return { "mov64", Form::Two, X, X, N };
				case LDRD:       return { "ldrd", Form::Load, X, R, N };
				case STRD:       return { "strd", Form::Store, R, X, N };
				case LDRDX:      return { "ldrd", Form::LoadIndexed, X, R, R };
				case STRDX:      return { "strd", Form::StoreIndexed, R, X, R };
				case LDRDP:      return { "ldrd", Form::LoadPc, X, N, N };
				case FADDD:      return { "fadd.d", Form::Three, D, D, D };
				case FSUBD:      return { "fsub.d", Form::Three, D, D, D };
				case FMULD:      return { "fmul.d", Form::Three, D, D, D };
				case FDIVD:      return { "fdiv.d", Form::Three, D, D, D };
				case FMAD:       return { "fma.d", Form::Three, D, D, D };
				case FSQRTD:     return { "fsqrt.d", Form::Two, D, D, N };
				case FCMPD:      return { "fcmp.d", Form::Compare, N, D, D };
				case FMINMAXD:   return { "", Form::Three, D, D, D };
				case FMODD:      return { "fmod.d", Form::Three, D, D, D };
				case FUNARYD:    return { "", Form::Two, D, D, N };
				case FCOPYSIGND: return { "fcopysign.d", Form::Three, D, D, D };
				case FCLASSD:    return { "fclass.d", Form::Two, R, D, N };
				case FCVT:       return { "", Form::Two, N, N, N };
				case FMOVD:      return { "fmov.d", Form::Two, D, D, N };
				case FLDRD:      return { "fldr.d", Form::Load, D, R, N };
				case FSTRD:      return { "fstr.d", Form::Store, R, D, N };
				case FLDRDX:     return { "fldr.d", Form::LoadIndexed, D, R, R };
				case FSTRDX:     return { "fstr.d", Form::StoreIndexed, R, D, R };
				case FLDRDP:     return { "fldr.d", Form::LoadPc, D, N, N };
				case MTFD:       return { "mtf.d", Form::Two, D, X, N };
				case MFFD:       return { "mff.d", Form::Two, X, D, N };
				default:         return {};
			}
		}

		// Whether a subfield value names an instruction; always for the opcodes that have none.
		constexpr bool validSubfield(Instruction instruction) noexcept
		{
			const u32 raw = instruction.raw();
			switch (instruction.opcode())
			{
				case Opcode::SHI64:   return fields::ShiftKind::get(raw) < ShiftNames.size();
				case Opcode::BITS64:  return fields::BitOp::get(raw) < BitOpNames.size();
				case Opcode::FUNARYD: return fields::UnaryOp::get(raw) < UnaryNames.size();
				case Opcode::FCVT:    return fields::FcvtKind::get(raw) < FcvtKindCount;
				default:              return true;
			}
		}
	}

	// The instruction a 64-bit word is: its name with the subfield applied and the bank of each field. Empty name for a
	// word that is not one (a subfield no instruction has, or an opcode that is not a 64-bit one).
	constexpr Entry describe(Instruction instruction) noexcept
	{
		if (!isWide(instruction.opcode()) || !detail::validSubfield(instruction))
			return {};
		Entry entry = detail::fixed(instruction.opcode());
		const u32 raw = instruction.raw();
		switch (instruction.opcode())
		{
			case Opcode::SHI64:    entry.name = ShiftNames[fields::ShiftKind::get(raw)]; break;
			case Opcode::BITS64:   entry.name = BitOpNames[fields::BitOp::get(raw)]; break;
			case Opcode::FMINMAXD: entry.name = fields::MinMax::get(raw) == 0 ? "fmin.d" : "fmax.d"; break;
			case Opcode::FUNARYD:  entry.name = UnaryNames[fields::UnaryOp::get(raw)]; break;
			case Opcode::FCVT:
			{
				const Conversion& conversion = Conversions[fields::FcvtKind::get(raw)];
				entry.name = conversion.name;
				entry.rd = conversion.to;
				entry.rs = conversion.from;
				break;
			}
			default: break;
		}
		return entry;
	}

	// Whether a register field is a valid register of its bank: an even number for a pair, and never r14 for an
	// integer pair.
	constexpr bool validField(Bank bank, u32 field) noexcept
	{
		switch (bank)
		{
			case Bank::IntPair:    return (field & 1u) == 0 && field != 14;
			case Bank::DoublePair: return (field & 1u) == 0;
			default:               return true;
		}
	}

	// Whether the machine runs this word, and why not (plan/v2 SPEC 5.4): a subfield no instruction has is BadSubfield,
	// a pair field that is odd or x7 is RegisterPair. Ok for every word that is not a 64-bit instruction.
	constexpr Decode check(Instruction instruction) noexcept
	{
		if (!isWide(instruction.opcode()))
			return Decode::Ok;
		if (!detail::validSubfield(instruction))
			return Decode::BadSubfield;
		const Entry entry = describe(instruction);
		if (!validField(entry.rd, instruction.rd()) || !validField(entry.rs, instruction.rs()) || !validField(entry.rt, instruction.rt()))
			return Decode::RegisterPair;
		return Decode::Ok;
	}
}

#pragma once

#include "opcodes.h"
#include "address.h"
#include <compare>
#include <span>

namespace ceres::vm
{
	class Instruction
	{
	public:
		using RawType = u32;

		static inline constexpr usize Size = sizeof(RawType);
		static inline constexpr Address SizeInBytes = Address(Size);

	private:
		static inline constexpr RawType OpcodeMask = 0xFF000000;
		static inline constexpr RawType Imm24Mask = 0x00FFFFFF;
		static inline constexpr RawType Imm16Mask = 0x0000FFFF;
		static inline constexpr RawType Imm8Mask = 0x000000FF;
		static inline constexpr RawType RdMask = 0x00F00000;
		static inline constexpr RawType RsMask = 0x000F0000;
		static inline constexpr RawType RtMask = 0x0000F000;
		static inline constexpr RawType RegisterMask = 0x0F; // 4 bits for register indices
		static inline constexpr RawType OpcodeShift = 24;
		static inline constexpr RawType RdShift = 20;
		static inline constexpr RawType RsShift = 16;
		static inline constexpr RawType RtShift = 12;

	private:
		RawType _raw = 0;

	public:
		constexpr Instruction() noexcept = default;
		constexpr Instruction(const Instruction&) noexcept = default;
		constexpr Instruction(Instruction&&) noexcept = default;
		constexpr ~Instruction() noexcept = default;

		constexpr Instruction& operator=(const Instruction&) noexcept = default;
		constexpr Instruction& operator=(Instruction&&) noexcept = default;

		constexpr bool operator==(const Instruction&) const noexcept = default;
		constexpr auto operator<=>(const Instruction&) const noexcept = default;

	public:
		forceinline constexpr Instruction(RawType raw) noexcept : _raw(raw) {}

		forceinline constexpr RawType raw() const noexcept { return _raw; }

		forceinline constexpr Opcode opcode() const noexcept { return static_cast<Opcode>((_raw & OpcodeMask) >> OpcodeShift); }
		forceinline constexpr u24 imm24() const noexcept { return static_cast<u24>(_raw & Imm24Mask); }
        forceinline constexpr i24 simm24() const noexcept { return static_cast<i24>(_raw & Imm24Mask); }
		forceinline constexpr u16 imm16() const noexcept { return _raw & Imm16Mask; }
		forceinline constexpr i16 simm16() const noexcept { return static_cast<i16>(_raw & Imm16Mask); }
		forceinline constexpr u8 imm8() const noexcept { return _raw & Imm8Mask; }
		forceinline constexpr u8 rd() const noexcept { return (_raw & RdMask) >> RdShift; }
		forceinline constexpr u8 rs() const noexcept { return (_raw & RsMask) >> RsShift; }
		forceinline constexpr u8 rt() const noexcept { return (_raw & RtMask) >> RtShift; }

	public:
		forceinline operator RawType() const noexcept { return _raw; }

	public:
		static constexpr Instruction make(Opcode opcode) noexcept
		{
			return Instruction(static_cast<RawType>(opcode) << OpcodeShift);
		}

		static constexpr Instruction make(Opcode opcode, u24 imm24) noexcept
		{
			return Instruction((static_cast<RawType>(opcode) << OpcodeShift) | (static_cast<u32>(imm24) & Imm24Mask));
		}
		static constexpr Instruction make(Opcode opcode, i24 simm24) noexcept
		{
			return Instruction((static_cast<RawType>(opcode) << OpcodeShift) | (static_cast<u32>(simm24) & Imm24Mask));
		}

		static constexpr Instruction make(Opcode opcode, u8 imm8) noexcept
		{
			return Instruction((static_cast<RawType>(opcode) << OpcodeShift) | (imm8 & Imm8Mask));
		}

		static constexpr Instruction make(Opcode opcode, u8 rd, u8 rs, u16 imm16 = 0) noexcept
		{
			return Instruction((static_cast<RawType>(opcode) << OpcodeShift) |
				((rd & RegisterMask) << RdShift) |
				((rs & RegisterMask) << RsShift) |
				((imm16 & Imm16Mask)));
		}

		static constexpr Instruction make(Opcode opcode, u8 rd, u8 rs, u8 rt, u8 imm8 = 0) noexcept
		{
			return Instruction((static_cast<RawType>(opcode) << OpcodeShift) |
				((rd & RegisterMask) << RdShift) |
				((rs & RegisterMask) << RsShift) |
				((rt & RegisterMask) << RtShift) |
				((imm8 & Imm8Mask)));
		}

		static constexpr std::span<const u8> asBytes(std::span<const Instruction> instructions) noexcept
		{
			return std::span<const u8>(reinterpret_cast<const u8*>(instructions.data()), instructions.size() * Size);
		}

	public:
       static constexpr Instruction NOP() noexcept { return make(Opcode::NOP); }

		// System Control
		static constexpr Instruction HALT() noexcept { return make(Opcode::HALT); }
		static constexpr Instruction TRAP() noexcept { return make(Opcode::TRAP); }
		static constexpr Instruction RESET() noexcept { return make(Opcode::RESET); }
		static constexpr Instruction INT(u8 imm8) noexcept { return make(Opcode::INT, 0, 0, 0, imm8); }
		static constexpr Instruction IRET() noexcept { return make(Opcode::IRET); }

		// Arithmetic
		static constexpr Instruction ADD(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::ADD, rd, rs, rt); }
		static constexpr Instruction ADDI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::ADDI, rd, rs, imm16); }
		static constexpr Instruction ADDC(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::ADDC, rd, rs, rt); }
		static constexpr Instruction ADDCI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::ADDCI, rd, rs, imm16); }
		static constexpr Instruction SUB(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::SUB, rd, rs, rt); }
		static constexpr Instruction SUBI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::SUBI, rd, rs, imm16); }
		static constexpr Instruction SUBC(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::SUBC, rd, rs, rt); }
		static constexpr Instruction SUBCI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::SUBCI, rd, rs, imm16); }
		static constexpr Instruction MUL(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::MUL, rd, rs, rt); }
		static constexpr Instruction MULI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::MULI, rd, rs, imm16); }
		static constexpr Instruction IMUL(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::IMUL, rd, rs, rt); }
		static constexpr Instruction IMULI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::IMULI, rd, rs, imm16); }
		static constexpr Instruction DIV(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::DIV, rd, rs, rt); }
		static constexpr Instruction DIVI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::DIVI, rd, rs, imm16); }
		static constexpr Instruction IDIV(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::IDIV, rd, rs, rt); }
		static constexpr Instruction IDIVI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::IDIVI, rd, rs, imm16); }
		static constexpr Instruction MOD(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::MOD, rd, rs, rt); }
		static constexpr Instruction MODI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::MODI, rd, rs, imm16); }
		static constexpr Instruction IMOD(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::IMOD, rd, rs, rt); }
		static constexpr Instruction IMODI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::IMODI, rd, rs, imm16); }

		// Bitwise Logic
		static constexpr Instruction AND(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::AND, rd, rs, rt); }
		static constexpr Instruction ANDI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::ANDI, rd, rs, imm16); }
		static constexpr Instruction OR(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::OR, rd, rs, rt); }
		static constexpr Instruction ORI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::ORI, rd, rs, imm16); }
		static constexpr Instruction XOR(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::XOR, rd, rs, rt); }
		static constexpr Instruction XORI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::XORI, rd, rs, imm16); }
		static constexpr Instruction NOT(u8 rd, u8 rs) noexcept { return make(Opcode::NOT, rd, rs); }
		static constexpr Instruction SHL(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::SHL, rd, rs, rt); }
		static constexpr Instruction SHLI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::SHLI, rd, rs, imm16); }
		static constexpr Instruction SHR(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::SHR, rd, rs, rt); }
		static constexpr Instruction SHRI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::SHRI, rd, rs, imm16); }
		static constexpr Instruction SAR(u8 rd, u8 rs, u8 rt) noexcept { return make(Opcode::SAR, rd, rs, rt); }
		static constexpr Instruction SARI(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::SARI, rd, rs, imm16); }

		// Memory Access
		static constexpr Instruction MOV(u8 rd, u8 rs) noexcept { return make(Opcode::MOV, rd, rs); }
		static constexpr Instruction MOVI(u8 rd, u16 imm16) noexcept { return make(Opcode::MOVI, rd, 0, imm16); }
		static constexpr Instruction MOVIH(u8 rd, u16 imm16) noexcept { return make(Opcode::MOVIH, rd, 0, imm16); }
		static constexpr Instruction LDRB(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::LDRB, rd, rs, imm16); }
		static constexpr Instruction ILDRB(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::ILDRB, rd, rs, imm16); }
		static constexpr Instruction LDRW(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::LDRW, rd, rs, imm16); }
		static constexpr Instruction ILDRW(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::ILDRW, rd, rs, imm16); }
		static constexpr Instruction LDRD(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::LDRD, rd, rs, imm16); }
		static constexpr Instruction LEA(u8 rd, u8 rs, u16 imm16) noexcept { return make(Opcode::LEA, rd, rs, imm16); }
		static constexpr Instruction STRB(u8 rs, u8 rt, u16 imm16) noexcept { return make(Opcode::STRB, rs, rt, imm16); }
		static constexpr Instruction STRW(u8 rs, u8 rt, u16 imm16) noexcept { return make(Opcode::STRW, rs, rt, imm16); }
		static constexpr Instruction STRD(u8 rs, u8 rt, u16 imm16) noexcept { return make(Opcode::STRD, rs, rt, imm16); }

		// Control Flow
		static constexpr Instruction JP(i24 simm24) noexcept { return make(Opcode::JP, simm24); }
		static constexpr Instruction JPR(u8 rs) noexcept { return make(Opcode::JPR, 0, rs); }
		static constexpr Instruction CMP(u8 rs, u8 rt) noexcept { return make(Opcode::CMP, 0, rs, rt); }
		static constexpr Instruction JZ(i24 simm24) noexcept { return make(Opcode::JZ, simm24); }
		static constexpr Instruction JZR(u8 rs) noexcept { return make(Opcode::JZR, 0, rs); }
		static constexpr Instruction JNZ(i24 simm24) noexcept { return make(Opcode::JNZ, simm24); }
		static constexpr Instruction JNZR(u8 rs) noexcept { return make(Opcode::JNZR, 0, rs); }
		static constexpr Instruction JC(i24 simm24) noexcept { return make(Opcode::JC, simm24); }
		static constexpr Instruction JCR(u8 rs) noexcept { return make(Opcode::JCR, 0, rs); }
		static constexpr Instruction JNC(i24 simm24) noexcept { return make(Opcode::JNC, simm24); }
		static constexpr Instruction JNCR(u8 rs) noexcept { return make(Opcode::JNCR, 0, rs); }
		static constexpr Instruction JS(i24 simm24) noexcept { return make(Opcode::JS, simm24); }
		static constexpr Instruction JSR(u8 rs) noexcept { return make(Opcode::JSR, 0, rs); }
		static constexpr Instruction JNS(i24 simm24) noexcept { return make(Opcode::JNS, simm24); }
		static constexpr Instruction JNSR(u8 rs) noexcept { return make(Opcode::JNSR, 0, rs); }
		static constexpr Instruction CALL(i24 simm24) noexcept { return make(Opcode::CALL, simm24); }
		static constexpr Instruction CALLR(u8 rs) noexcept { return make(Opcode::CALLR, 0, rs); }
		static constexpr Instruction RET() noexcept { return make(Opcode::RET); }

		// Stack Operations
		static constexpr Instruction PUSH(u8 rs) noexcept { return make(Opcode::PUSH, 0, rs); }
		static constexpr Instruction POP(u8 rd) noexcept { return make(Opcode::POP, rd, 0); }
		static constexpr Instruction PUSHF(u8 rs) noexcept { return make(Opcode::PUSHF, 0, rs); }
		static constexpr Instruction POPF(u8 rd) noexcept { return make(Opcode::POPF, rd, 0); }

		// I/O Operations
		static constexpr Instruction IN(u8 rd, u8 imm8) noexcept { return make(Opcode::IN, rd, 0, 0, imm8); }
		static constexpr Instruction INR(u8 rd, u8 rs) noexcept { return make(Opcode::INR, rd, rs); }
		static constexpr Instruction OUT(u8 rs, u8 imm8) noexcept { return make(Opcode::OUT, rs, 0, 0, imm8); }
		static constexpr Instruction OUTR(u8 rs, u8 rt) noexcept { return make(Opcode::OUTR, rs, rt); }

		// Miscellaneous - Reserved

	};
}

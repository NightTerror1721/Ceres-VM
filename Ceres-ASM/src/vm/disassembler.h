#pragma once

#include "instructions.h"
#include "opcodes.h"
#include "address.h"
#include <string>
#include <format>
#include <span>

namespace ceres::vm
{
	// Turns encoded words back into readable text. Its reason to exist is debugging the encoder:
	// a failing expectation reads as "expected LDRB r2, [r1 + 2]" instead of two hex words that
	// have to be decoded by hand.
	class Disassembler
	{
	private:
		// Which fields an opcode actually uses, and in what order they are written in assembly.
		enum class Shape : u8
		{
			Unknown,
			None,           // HALT
			Imm8,           // INT 3
			Rd,             // POP r1
			Rs,             // PUSH r1
			Fd,             // FPOP f1
			Fs,             // FPUSH f1
			RdRs,           // MOV r1, r2
			RsRt,           // CMP r1, r2
			FdFs,           // FMOV f1, f2
			FsFt,           // FCMP f1, f2
			FdRs,           // ITOF f1, r2
			RdFs,           // FTOI r1, f2
			RdRsRt,         // ADD r1, r2, r3
			FdFsFt,         // FADD f1, f2, f3
			RdRsImm16,      // ADDI r1, r2, 5
			RdImm16,        // LI r1, 5
			RsImm16,        // CMPI r1, 5
			Load,           // LDR r1, [r2 + 4]
			FLoad,          // FLDR f1, [r2 + 4]
			Store,          // STR [r1 + 4], r2   (base in Rd, value in Rs)
			FStore,         // FSTR [r1 + 4], f2
			Lea,            // LEA r1, [r2 + 4]
			Simm24,         // JP +8
			Imm16,          // ENTER 24
			Mask16,         // PUSHM 0x0f00
			IndexedLoad,    // LDRX r1, [r2 + r3]
			IndexedFLoad,   // FLDRX f1, [r2 + r3]
			IndexedStore,   // STRX [r1 + r3], r2
			IndexedFStore,  // FSTRX [r1 + r3], f2
			PCLoad,         // LDRP r1, [pc + 128]
			PCFLoad,        // FLDRP f1, [pc + 128]
			PCStore,        // STRP [pc + 128], r1
			PCFStore,       // FSTRP [pc + 128], f1
			RdImm8,         // IN r1, 0x01
			RsImm8,         // OUT 0x01, r1
			RdRsImm8,       // INM 0x01, r1, r2
			RsRtImm8,       // OUTM 0x01, r1, r2
			RdRsPort,       // INR r1, r2
			RsRtPort,       // OUTR r1, r2
			RdRsRtPort,     // INRM / OUTRM
		};

		struct Entry
		{
			std::string_view name = "???";
			Shape shape = Shape::Unknown;
		};

		static constexpr Entry describe(Opcode opcode) noexcept
		{
			switch (opcode)
			{
				case Opcode::NOP:    return { "NOP",   Shape::None };
				case Opcode::HALT:   return { "HALT",  Shape::None };
				case Opcode::TRAP:   return { "TRAP",  Shape::None };
				case Opcode::RESET:  return { "RESET", Shape::None };
				case Opcode::INT:    return { "INT",   Shape::Imm8 };
				case Opcode::IRET:   return { "IRET",  Shape::None };
				case Opcode::CLI:    return { "CLI",   Shape::None };
				case Opcode::STI:    return { "STI",   Shape::None };

				case Opcode::ADD:    return { "ADD",   Shape::RdRsRt };
				case Opcode::ADDI:   return { "ADDI",  Shape::RdRsImm16 };
				case Opcode::ADDC:   return { "ADDC",  Shape::RdRsRt };
				case Opcode::ADDCI:  return { "ADDCI", Shape::RdRsImm16 };
				case Opcode::FADD:   return { "FADD",  Shape::FdFsFt };
				case Opcode::SUB:    return { "SUB",   Shape::RdRsRt };
				case Opcode::SUBI:   return { "SUBI",  Shape::RdRsImm16 };
				case Opcode::SUBC:   return { "SUBC",  Shape::RdRsRt };
				case Opcode::SUBCI:  return { "SUBCI", Shape::RdRsImm16 };
				case Opcode::FSUB:   return { "FSUB",  Shape::FdFsFt };
				case Opcode::MUL:    return { "MUL",   Shape::RdRsRt };
				case Opcode::MULI:   return { "MULI",  Shape::RdRsImm16 };
				case Opcode::IMUL:   return { "IMUL",  Shape::RdRsRt };
				case Opcode::IMULI:  return { "IMULI", Shape::RdRsImm16 };
				case Opcode::FMUL:   return { "FMUL",  Shape::FdFsFt };
				case Opcode::DIV:    return { "DIV",   Shape::RdRsRt };
				case Opcode::DIVI:   return { "DIVI",  Shape::RdRsImm16 };
				case Opcode::IDIV:   return { "IDIV",  Shape::RdRsRt };
				case Opcode::IDIVI:  return { "IDIVI", Shape::RdRsImm16 };
				case Opcode::FDIV:   return { "FDIV",  Shape::FdFsFt };
				case Opcode::MOD:    return { "MOD",   Shape::RdRsRt };
				case Opcode::MODI:   return { "MODI",  Shape::RdRsImm16 };
				case Opcode::IMOD:   return { "IMOD",  Shape::RdRsRt };
				case Opcode::IMODI:  return { "IMODI", Shape::RdRsImm16 };
				case Opcode::FNEG:   return { "FNEG",  Shape::FdFs };

				case Opcode::AND:    return { "AND",   Shape::RdRsRt };
				case Opcode::ANDI:   return { "ANDI",  Shape::RdRsImm16 };
				case Opcode::OR:     return { "OR",    Shape::RdRsRt };
				case Opcode::ORI:    return { "ORI",   Shape::RdRsImm16 };
				case Opcode::XOR:    return { "XOR",   Shape::RdRsRt };
				case Opcode::XORI:   return { "XORI",  Shape::RdRsImm16 };
				case Opcode::NOT:    return { "NOT",   Shape::RdRs };
				case Opcode::SHL:    return { "SHL",   Shape::RdRsRt };
				case Opcode::SHLI:   return { "SHLI",  Shape::RdRsImm16 };
				case Opcode::SHR:    return { "SHR",   Shape::RdRsRt };
				case Opcode::SHRI:   return { "SHRI",  Shape::RdRsImm16 };
				case Opcode::SAR:    return { "SAR",   Shape::RdRsRt };
				case Opcode::SARI:   return { "SARI",  Shape::RdRsImm16 };

				case Opcode::MOV:    return { "MOV",   Shape::RdRs };
				case Opcode::FMOV:   return { "FMOV",  Shape::FdFs };
				case Opcode::LI:     return { "LI",    Shape::RdImm16 };
				case Opcode::LUI:    return { "LUI",   Shape::RdImm16 };
				case Opcode::LDR:    return { "LDR",   Shape::Load };
				case Opcode::LDRB:   return { "LDRB",  Shape::Load };
				case Opcode::LDRH:   return { "LDRH",  Shape::Load };
				case Opcode::LDRSB:  return { "LDRSB", Shape::Load };
				case Opcode::LDRSH:  return { "LDRSH", Shape::Load };
				case Opcode::FLDR:   return { "FLDR",  Shape::FLoad };
				case Opcode::STR:    return { "STR",   Shape::Store };
				case Opcode::STRB:   return { "STRB",  Shape::Store };
				case Opcode::STRH:   return { "STRH",  Shape::Store };
				case Opcode::FSTR:   return { "FSTR",  Shape::FStore };
				case Opcode::LEA:    return { "LEA",   Shape::Lea };

				case Opcode::JP:     return { "JP",    Shape::Simm24 };
				case Opcode::JPR:    return { "JPR",   Shape::Rs };
				case Opcode::CMP:    return { "CMP",   Shape::RsRt };
				case Opcode::CMPI:   return { "CMPI",  Shape::RsImm16 };
				case Opcode::FCMP:   return { "FCMP",  Shape::FsFt };
				case Opcode::JZ:     return { "JZ",    Shape::Simm24 };
				case Opcode::JZR:    return { "JZR",   Shape::Rs };
				case Opcode::JNZ:    return { "JNZ",   Shape::Simm24 };
				case Opcode::JNZR:   return { "JNZR",  Shape::Rs };
				case Opcode::JC:     return { "JC",    Shape::Simm24 };
				case Opcode::JCR:    return { "JCR",   Shape::Rs };
				case Opcode::JNC:    return { "JNC",   Shape::Simm24 };
				case Opcode::JNCR:   return { "JNCR",  Shape::Rs };
				case Opcode::JS:     return { "JS",    Shape::Simm24 };
				case Opcode::JSR:    return { "JSR",   Shape::Rs };
				case Opcode::JNS:    return { "JNS",   Shape::Simm24 };
				case Opcode::JNSR:   return { "JNSR",  Shape::Rs };
				case Opcode::CALL:   return { "CALL",  Shape::Simm24 };
				case Opcode::CALLR:  return { "CALLR", Shape::Rs };
				case Opcode::RET:    return { "RET",   Shape::None };
				case Opcode::JO:     return { "JO",    Shape::Simm24 };
				case Opcode::JOR:    return { "JOR",   Shape::Rs };
				case Opcode::JNO:    return { "JNO",   Shape::Simm24 };
				case Opcode::JNOR:   return { "JNOR",  Shape::Rs };
				case Opcode::JGR:     return { "JGR", Shape::Simm24 };
				case Opcode::JGRR:    return { "JGRR", Shape::Rs };
				case Opcode::JGE:     return { "JGE", Shape::Simm24 };
				case Opcode::JGER:    return { "JGER", Shape::Rs };
				case Opcode::JLS:     return { "JLS", Shape::Simm24 };
				case Opcode::JLSR:    return { "JLSR", Shape::Rs };
				case Opcode::JLE:     return { "JLE", Shape::Simm24 };
				case Opcode::JLER:    return { "JLER", Shape::Rs };
				case Opcode::JAB:     return { "JAB", Shape::Simm24 };
				case Opcode::JABR:    return { "JABR", Shape::Rs };
				case Opcode::JAE:     return { "JAE", Shape::Simm24 };
				case Opcode::JAER:    return { "JAER", Shape::Rs };
				case Opcode::JBL:     return { "JBL", Shape::Simm24 };
				case Opcode::JBLR:    return { "JBLR", Shape::Rs };
				case Opcode::JBE:     return { "JBE", Shape::Simm24 };
				case Opcode::JBER:    return { "JBER", Shape::Rs };

				case Opcode::PUSH:   return { "PUSH",  Shape::Rs };
				case Opcode::POP:    return { "POP",   Shape::Rd };
				case Opcode::PUSHF:  return { "PUSHF", Shape::None };
				case Opcode::POPF:   return { "POPF",  Shape::None };
				case Opcode::FPUSH:  return { "FPUSH", Shape::Fs };
				case Opcode::FPOP:   return { "FPOP",  Shape::Fd };
				case Opcode::LDRX:    return { "LDRX",    Shape::IndexedLoad };
				case Opcode::LDRBX:   return { "LDRBX",   Shape::IndexedLoad };
				case Opcode::LDRHX:   return { "LDRHX",   Shape::IndexedLoad };
				case Opcode::LDRSBX:  return { "LDRSBX",  Shape::IndexedLoad };
				case Opcode::LDRSHX:  return { "LDRSHX",  Shape::IndexedLoad };
				case Opcode::FLDRX:  return { "FLDRX",  Shape::IndexedFLoad };
				case Opcode::STRX:    return { "STRX",    Shape::IndexedStore };
				case Opcode::STRBX:   return { "STRBX",   Shape::IndexedStore };
				case Opcode::STRHX:   return { "STRHX",   Shape::IndexedStore };
				case Opcode::FSTRX:  return { "FSTRX",  Shape::IndexedFStore };
				case Opcode::LDRP:    return { "LDRP",    Shape::PCLoad };
				case Opcode::LDRBP:   return { "LDRBP",   Shape::PCLoad };
				case Opcode::LDRHP:   return { "LDRHP",   Shape::PCLoad };
				case Opcode::LDRSBP:  return { "LDRSBP",  Shape::PCLoad };
				case Opcode::LDRSHP:  return { "LDRSHP",  Shape::PCLoad };
				case Opcode::FLDRP:  return { "FLDRP",  Shape::PCFLoad };
				case Opcode::STRP:    return { "STRP",    Shape::PCStore };
				case Opcode::STRBP:   return { "STRBP",   Shape::PCStore };
				case Opcode::STRHP:   return { "STRHP",   Shape::PCStore };
				case Opcode::FSTRP:  return { "FSTRP",  Shape::PCFStore };
				case Opcode::ENTER:  return { "ENTER", Shape::Imm16 };
				case Opcode::LEAVE:  return { "LEAVE", Shape::None };
				case Opcode::PUSHM:  return { "PUSHM", Shape::Mask16 };
				case Opcode::POPM:   return { "POPM",  Shape::Mask16 };

				case Opcode::ITOF:   return { "ITOF",  Shape::FdRs };
				case Opcode::IITOF:  return { "IITOF", Shape::FdRs };
				case Opcode::FTOI:   return { "FTOI",  Shape::RdFs };
				case Opcode::FTOII:  return { "FTOII", Shape::RdFs };
				case Opcode::MTF:    return { "MTF",   Shape::FdRs };
				case Opcode::MFF:    return { "MFF",   Shape::RdFs };

				case Opcode::IN:     return { "IN",    Shape::RdImm8 };
				case Opcode::INB:    return { "INB",   Shape::RdImm8 };
				case Opcode::INH:    return { "INH",   Shape::RdImm8 };
				case Opcode::INSB:   return { "INSB",  Shape::RdImm8 };
				case Opcode::INSH:   return { "INSH",  Shape::RdImm8 };
				case Opcode::INM:    return { "INM",   Shape::RdRsImm8 };
				case Opcode::INR:    return { "INR",   Shape::RdRsPort };
				case Opcode::INRB:   return { "INRB",  Shape::RdRsPort };
				case Opcode::INRH:   return { "INRH",  Shape::RdRsPort };
				case Opcode::INRSB:  return { "INRSB", Shape::RdRsPort };
				case Opcode::INRSH:  return { "INRSH", Shape::RdRsPort };
				case Opcode::INRM:   return { "INRM",  Shape::RdRsRtPort };
				case Opcode::OUT:    return { "OUT",   Shape::RsImm8 };
				case Opcode::OUTB:   return { "OUTB",  Shape::RsImm8 };
				case Opcode::OUTH:   return { "OUTH",  Shape::RsImm8 };
				case Opcode::OUTM:   return { "OUTM",  Shape::RsRtImm8 };
				case Opcode::OUTR:   return { "OUTR",  Shape::RsRtPort };
				case Opcode::OUTRB:  return { "OUTRB", Shape::RsRtPort };
				case Opcode::OUTRH:  return { "OUTRH", Shape::RsRtPort };
				case Opcode::OUTRM:  return { "OUTRM", Shape::RdRsRtPort };

				default: return {};
			}
		}

	public:
		// One instruction as text. Unknown opcodes come back as their raw byte so a decoding
		// mistake is visible instead of silently rendering as something plausible.
		static std::string disassemble(Instruction instruction)
		{
			const Opcode opcode = instruction.opcode();
			const Entry entry = describe(opcode);

			const u8 rd = instruction.rd();
			const u8 rs = instruction.rs();
			const u8 rt = instruction.rt();
			const u16 imm16 = instruction.imm16();
			const i16 simm16 = instruction.simm16();
			const u8 imm8 = instruction.imm8();
			const i32 simm24 = instruction.simm24().signedValue();

			switch (entry.shape)
			{
				case Shape::None:       return std::string(entry.name);
				case Shape::Imm8:       return std::format("{} {}", entry.name, imm8);
				case Shape::Rd:         return std::format("{} r{}", entry.name, rd);
				case Shape::Rs:         return std::format("{} r{}", entry.name, rs);
				case Shape::Fd:         return std::format("{} f{}", entry.name, rd);
				case Shape::Fs:         return std::format("{} f{}", entry.name, rs);
				case Shape::RdRs:       return std::format("{} r{}, r{}", entry.name, rd, rs);
				case Shape::RsRt:       return std::format("{} r{}, r{}", entry.name, rs, rt);
				case Shape::FdFs:       return std::format("{} f{}, f{}", entry.name, rd, rs);
				case Shape::FsFt:       return std::format("{} f{}, f{}", entry.name, rs, rt);
				case Shape::FdRs:       return std::format("{} f{}, r{}", entry.name, rd, rs);
				case Shape::RdFs:       return std::format("{} r{}, f{}", entry.name, rd, rs);
				case Shape::RdRsRt:     return std::format("{} r{}, r{}, r{}", entry.name, rd, rs, rt);
				case Shape::FdFsFt:     return std::format("{} f{}, f{}, f{}", entry.name, rd, rs, rt);
				case Shape::RdRsImm16:  return std::format("{} r{}, r{}, {}", entry.name, rd, rs, imm16);
				case Shape::RdImm16:    return std::format("{} r{}, {}", entry.name, rd, imm16);
				case Shape::RsImm16:    return std::format("{} r{}, {}", entry.name, rs, simm16);
				// A displacement is signed, so it is rendered with its own sign rather than a
				// hard-coded '+' and a number that has wrapped.
				case Shape::Load:       return std::format("{} r{}, [r{} {} {}]", entry.name, rd, rs, simm16 < 0 ? '-' : '+', std::abs(static_cast<int>(simm16)));
				case Shape::FLoad:      return std::format("{} f{}, [r{} {} {}]", entry.name, rd, rs, simm16 < 0 ? '-' : '+', std::abs(static_cast<int>(simm16)));
				case Shape::Store:      return std::format("{} [r{} {} {}], r{}", entry.name, rd, simm16 < 0 ? '-' : '+', std::abs(static_cast<int>(simm16)), rs);
				case Shape::FStore:     return std::format("{} [r{} {} {}], f{}", entry.name, rd, simm16 < 0 ? '-' : '+', std::abs(static_cast<int>(simm16)), rs);
				case Shape::Lea:        return std::format("{} r{}, [r{} {} {}]", entry.name, rd, rs, simm16 < 0 ? '-' : '+', std::abs(static_cast<int>(simm16)));
				case Shape::Simm24:     return std::format("{} {}{}", entry.name, simm24 < 0 ? "" : "+", simm24);
				case Shape::Imm16:      return std::format("{} {}", entry.name, imm16);
				case Shape::Mask16:     return std::format("{} {:#06x}", entry.name, imm16);
				case Shape::IndexedLoad:   return std::format("{} r{}, [r{} + r{}]", entry.name, rd, rs, rt);
				case Shape::IndexedFLoad:  return std::format("{} f{}, [r{} + r{}]", entry.name, rd, rs, rt);
				case Shape::IndexedStore:  return std::format("{} [r{} + r{}], r{}", entry.name, rd, rt, rs);
				case Shape::IndexedFStore: return std::format("{} [r{} + r{}], f{}", entry.name, rd, rt, rs);
				case Shape::PCLoad:     return std::format("{} r{}, [pc {} {}]", entry.name, rd, simm16 < 0 ? '-' : '+', std::abs(static_cast<int>(simm16)));
				case Shape::PCFLoad:    return std::format("{} f{}, [pc {} {}]", entry.name, rd, simm16 < 0 ? '-' : '+', std::abs(static_cast<int>(simm16)));
				case Shape::PCStore:    return std::format("{} [pc {} {}], r{}", entry.name, simm16 < 0 ? '-' : '+', std::abs(static_cast<int>(simm16)), rs);
				case Shape::PCFStore:   return std::format("{} [pc {} {}], f{}", entry.name, simm16 < 0 ? '-' : '+', std::abs(static_cast<int>(simm16)), rs);
				case Shape::RdImm8:     return std::format("{} r{}, {:#04x}", entry.name, rd, imm8);
				case Shape::RsImm8:     return std::format("{} {:#04x}, r{}", entry.name, imm8, rs);
				case Shape::RdRsImm8:   return std::format("{} {:#04x}, r{}, r{}", entry.name, imm8, rd, rs);
				case Shape::RsRtImm8:   return std::format("{} {:#04x}, r{}, r{}", entry.name, imm8, rs, rt);
				case Shape::RdRsPort:   return std::format("{} r{}, r{}", entry.name, rd, rs);
				case Shape::RsRtPort:   return std::format("{} r{}, r{}", entry.name, rt, rs);
				case Shape::RdRsRtPort: return std::format("{} r{}, r{}, r{}", entry.name, rd, rs, rt);

				default:
					return std::format("<unknown opcode {:#04x}>", static_cast<u8>(opcode));
			}
		}

		// Address, encoded word and text, one instruction per line.
		static std::string listing(std::span<const u8> text, Address base = Address(0))
		{
			std::string out;
			out.reserve(text.size() * 12);

			const usize count = text.size() / Instruction::Size;
			for (usize i = 0; i < count; ++i)
			{
				const usize offset = i * Instruction::Size;
				const Instruction::RawType raw =
					static_cast<Instruction::RawType>(text[offset]) |
					(static_cast<Instruction::RawType>(text[offset + 1]) << 8) |
					(static_cast<Instruction::RawType>(text[offset + 2]) << 16) |
					(static_cast<Instruction::RawType>(text[offset + 3]) << 24);

				out += std::format("{:08x}  {:08x}  {}\n",
					base.value() + static_cast<u32>(offset), raw, disassemble(Instruction(raw)));
			}

			// A trailing partial word means the text section is not a whole number of instructions.
			if (const usize remainder = text.size() % Instruction::Size; remainder != 0)
				out += std::format("{:08x}  <{} trailing byte(s)>\n",
					base.value() + static_cast<u32>(count * Instruction::Size), remainder);

			return out;
		}
	};
}

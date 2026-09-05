#pragma once

#include "common_defs.h"

namespace ceres::casm
{
	enum class Mnemonic : u8
	{
		NOP, // NOP
		HALT, // HALT
		TRAP, // TRAP
		RESET, // RESET
		INT, // INT
		IRET, // IRET
		CLI, // CLI
		STI, // STI

		ADD, // ADD, ADDI, FADD
		ADC, // ADDC, ADDCI
		SUB, // SUB, SUBI, FSUB
		SBC, // SUBC, SUBCI
		MUL, // MUL, MULI, FMUL
		IMUL, // IMUL, IMULI
		DIV, // DIV, DIVI, FDIV
		IDIV, // IDIV, IDIVI
		MOD, // MOD, MODI
		IMOD, // IMOD, IMODI
		NEG, // Pseudo-instruction // IMUL rd rs -1 | FNEG

		AND, // AND, ANDI
		OR, // OR, ORI
		XOR, // XOR, XORI
		NOT, // NOT
		SHL, // SHL, SHLI
		SHR, // SHR, SHRI
		SAR, // SAR, SARI

		MOV, // MOV, MOVI, FMOV, LDRB, LDRW, LDRD, FLDR, STRB, STRW, STRD, FSTR
		LI, // LI
		LUI, // LUI
		LDR, // LDR, FLDR
		LDRB, // LDRB,
		LDRH, // LDRH,
		LDRSB, // LDRSB,
		LDRSH, // LDRSH,
		LDV, // Pseudo-instruction // Load variable address: LUI + ORI + (LDR | LDRB | LDRH | LDRSB | LDRSH | FLDR)
		STR, // STR, FSTR
		STRB, // STRB,
		STRH, // STRH,
		STV, // Pseudo-instruction // Store variable address: LUI + ORI + (STR | STRB | STRH | FSTR)
		LA, // Pseudo-instruction // Load address: LUI + ORI
		LEA, // LEA

		JP, // JP, JPR
		CMP, // CMP, CMPI, FCMP
		JZ, // JZ, JZR
		JNZ, // JNZ, JNZR
		JC, // JC, JCR
		JNC, // JNC, JNCR
		JS, // JS, JSR
		JNS, // JNS, JNSR
		JO, // JO, JOR
		JNO, // JNO, JNOR
		CALL, // CALL, CALLR
		RET, // RET

		PUSH, // PUSH, FPUSH
		POP, // POP, FPOP
		PUSHF, // PUSHF
		POPF, // POPF

		ITOF, // ITOF
		IITOF, // IITOF
		FTOI, // FTOI
		FTOII, // FTOII
		MTF, // MTF
		MFF, // MFF

		IN, // IN, INR
		INB, // INB, INBR
		INH, // INH, INHR
		INSB, // INSB, INSBR
		INSH, // INSH, INSHR
		INM, // INM, INMR
		OUT, // OUT, OUTR
		OUTB, // OUTB, OUTBR
		OUTH, // OUTH, OUTHR
		OUTM // OUTM, OUTMR
	};

	std::string_view mnemonicToString(Mnemonic mnemonic) noexcept;
	std::optional<Mnemonic> stringToMnemonic(std::string_view str, bool caseSensitive = false) noexcept;
}

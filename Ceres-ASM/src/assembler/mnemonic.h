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
		INC, // Pseudo-instruction // ADDI rd, rd, 1
		DEC, // Pseudo-instruction // SUBI rd, rd, 1

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
		LC, // Pseudo-instruction // Load a full 32-bit constant: LUI + ORI
		CLR, // Pseudo-instruction // LI rd, 0
		SWAP, // Pseudo-instruction // three XORs, no temporary
		LEA, // LEA

		JP, // JP, JPR
		CMP, // CMP, CMPI, FCMP
		TST, // Pseudo-instruction // CMPI rs, 0
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
		JMP, // Pseudo-instruction // Alias of JP

		// Comparison jumps. JEQ/JNE are aliases of JZ/JNZ; the ordering ones have opcodes of their
		// own because no single-flag jump spells them.
		JEQ, // JZ, JZR
		JNE, // JNZ, JNZR
		JGR, // JGR, JGRR  (signed >)
		JGE, // JGE, JGER  (signed >=)
		JLS, // JLS, JLSR  (signed <)
		JLE, // JLE, JLER  (signed <=)
		JAB, // JAB, JABR  (unsigned >)
		JAE, // JAE, JAER  (unsigned >=)
		JBL, // JBL, JBLR  (unsigned <)
		JBE, // JBE, JBER  (unsigned <=)

		// Compare and jump in one written instruction: CMP/CMPI/FCMP followed by the matching jump.
		IFEQ, IFNE, IFGR, IFGE, IFLS, IFLE, IFAB, IFAE, IFBL, IFBE,

		PUSH, // PUSH, FPUSH
		POP, // POP, FPOP
		PUSHM, // PUSHM
		POPM, // POPM
		PUSHF, // PUSHF
		POPF, // POPF
		ENTER, // Pseudo-instruction // PUSH fp + MOV fp, sp
		LEAVE, // Pseudo-instruction // MOV sp, fp + POP fp

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

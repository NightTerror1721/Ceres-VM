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

		MTP, // MTP - move to PTBR (rs -> page directory base register)
		MFP, // MFP - move from PTBR (page directory base register -> rd)
		PGON, // PGON - enable paging
		PGOFF, // PGOFF - disable paging
		INVLPG, // INVLPG - invalidate one TLB entry
		FLPG, // FLPG - flush the whole TLB
		MFPF, // MFPF - move from page-fault address (rd = last faulting virtual address)

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
		FMOD, // FMOD
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

		MOV, // MOV, FMOV. Register to register only: a 16-bit immediate is LI and a 32-bit one is LA.
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
		// Putting an address in rd, whichever way it is written: the address of a symbol, a full
		// 32-bit literal (LUI + ORI for both), or `[rs + imm]` computed at run time (LEA, one word).
		// `lc` and `lea` still parse, as the spellings these had when they were separate.
		LA, // Pseudo-instruction, except for the [rs + imm] form which is a real opcode
		CLR, // Pseudo-instruction // LI rd, 0
		SWAP, // Pseudo-instruction // three XORs, no temporary
		MULH,
		IMULH,
		ABS,
		MIN,
		IMIN,
		MAX,
		IMAX,
		FMIN,
		FMAX,
		CLZ,
		CTZ,
		POPCNT,
		BSWAP,
		ROL,
		ROR,
		SXTB,
		SXTH,
		SQRT,
		FROUND, // FROUND
		FFLOOR, // FFLOOR
		FCEIL, // FCEIL
		FTRUNC, // FTRUNC
		FCOPYSIGN, // FCOPYSIGN
		FMA, // FMA. fd = fd + fs * ft: the accumulator is a source as well as the destination.
		FRECIPE, // FRECIPE
		FRSQRTE, // FRSQRTE
		FCLASS, // FCLASS
		LDVP, // PC-relative load of a variable within reach
		STVP, // PC-relative store of a variable within reach

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

		// With no operand at all it is the flags register, which nothing else can be confused with.
		PUSH, // PUSH, FPUSH, PUSHF
		POP, // POP, FPOP, POPF
		BL, // BL, BLR
		// Kept apart from PUSH/POP on purpose: `push 5` is an error today and would silently become
		// a register-mask push if the immediate form shared the name.
		PUSHM, // PUSHM
		POPM, // POPM
		ENTER, // Pseudo-instruction // PUSH fp + MOV fp, sp
		LEAVE, // Pseudo-instruction // MOV sp, fp + POP fp

		ITOF, // ITOF
		IITOF, // IITOF
		FTOI, // FTOI
		FTOII, // FTOII
		MTF, // MTF
		MFF, // MFF

	};

	std::string_view mnemonicToString(Mnemonic mnemonic) noexcept;
	std::optional<Mnemonic> stringToMnemonic(std::string_view str, bool caseSensitive = false) noexcept;
}

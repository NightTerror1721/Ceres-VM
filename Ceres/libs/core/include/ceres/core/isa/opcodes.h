#pragma once

#include <ceres/core/base/types.h>

namespace ceres::isa
{
	enum class Opcode : u8
	{
		// System Control //
		NOP = 0x00, // [] - No operation. The virtual machine does nothing and proceeds to the next instruction.
		HALT = 0x01, // [] - Halt the virtual machine until an interrupt is received.
		TRAP = 0x02, // [] - Trigger a trap, which is a synchronous exception that can be used for debugging or error handling.
		RESET = 0x03, // [] - Reset the virtual machine to its initial state.
		INT = 0x04, // [imm8] - Trigger an interrupt with the given interrupt number.
		IRET = 0x05, // [] - Return from an interrupt handler, restoring the previous state of the virtual machine.
		CLI = 0x06, // [] - Clear the interrupt flag, masking user interrupts.
		STI = 0x07, // [] - Set the interrupt flag, allowing user interrupts to be delivered.

		// Arithmetic //
		ADD = 0x10, // [rd, rs, rt] - rd = rs + rt
		ADDI = 0x11, // [rd, rs, imm16] - rd = rs + imm16
		ADDC = 0x12, // [rd, rs, rt] - rd = rs + rt + carry
		ADDCI = 0x13, // [rd, rs, imm16] - rd = rs + imm16 + carry
		FADD = 0x14, // [fd, fs, ft] - fd = fs + ft (floating-point add)
		SUB = 0x15, // [rd, rs, rt] - rd = rs - rt
		SUBI = 0x16, // [rd, rs, imm16] - rd = rs - imm16
		SUBC = 0x17, // [rd, rs, rt] - rd = rs - rt - borrow
		SUBCI = 0x18, // [rd, rs, imm16] - rd = rs - imm16 - borrow
		FSUB = 0x19, // [fd, fs, ft] - fd = fs - ft (floating-point subtract)
		MUL = 0x1A, // [rd, rs, rt] - rd = rs * rt
		MULI = 0x1B, // [rd, rs, imm16] - rd = rs * imm16
		IMUL = 0x1C, // [rd, rs, rt] - rd = rs * rt (signed)
		IMULI = 0x1D, // [rd, rs, simm16] - rd = rs * imm16 (signed)
		FMUL = 0x1E, // [fd, fs, ft] - fd = fs * ft (floating-point multiply)
		DIV = 0x1F, // [rd, rs, rt] - rd = rs / rt
		DIVI = 0x20, // [rd, rs, imm16] - rd = rs / imm16
		IDIV = 0x21, // [rd, rs, rt] - rd = rs / rt (signed)
		IDIVI = 0x22, // [rd, rs, simm16] - rd = rs / imm16 (signed)
		FDIV = 0x23, // [fd, fs, ft] - fd = fs / ft (floating-point divide)
		MOD = 0x24, // [rd, rs, rt] - rd = rs % rt
		MODI = 0x25, // [rd, rs, imm16] - rd = rs % imm16
		IMOD = 0x26, // [rd, rs, rt] - rd = rs % rt (signed)
		IMODI = 0x27, // [rd, rs, imm16] - rd = rs % imm16 (signed)
		FNEG = 0x28, // [fd, fs] - fd = -fs (floating-point negate)
		// MUL keeps the low 32 bits and drops the rest, so an overflow was simply lost. These are
		// the half that went missing.
		MULH = 0x29, // [rd, rs, rt] - rd = high 32 bits of rs * rt (unsigned)
		IMULH = 0x2A, // [rd, rs, rt] - rd = high 32 bits of rs * rt (signed)
		ABS = 0x2B, // [rd, rs] - rd = |rs| as a signed value; |INT_MIN| does not fit and sets Overflow
		MIN = 0x2C, // [rd, rs, rt] - rd = min(rs, rt) (unsigned)
		IMIN = 0x2D, // [rd, rs, rt] - rd = min(rs, rt) (signed)
		MAX = 0x2E, // [rd, rs, rt] - rd = max(rs, rt) (unsigned)
		IMAX = 0x2F, // [rd, rs, rt] - rd = max(rs, rt) (signed)

		// Bitwise Logic //
		AND = 0x30, // [rd, rs, rt] - rd = rs & rt
		ANDI = 0x31, // [rd, rs, imm16] - rd = rs & imm16
		OR = 0x32, // [rd, rs, rt] - rd = rs | rt
		ORI = 0x33, // [rd, rs, imm16] - rd = rs | imm16
		XOR = 0x34, // [rd, rs, rt] - rd = rs ^ rt
		XORI = 0x35, // [rd, rs, imm16] - rd = rs ^ imm16
		NOT = 0x36, // [rd, rs] - rd = ~rs
		SHL = 0x37, // [rd, rs, rt] - rd = rs << rt
		SHLI = 0x38, // [rd, rs, imm16] - rd = rs << imm16
		SHR = 0x39, // [rd, rs, rt] - rd = rs >> rt (logical)
		SHRI = 0x3A, // [rd, rs, imm16] - rd = rs >> imm16 (logical)
		SAR = 0x3B, // [rd, rs, rt] - rd = rs >> rt (arithmetic)
		SARI = 0x3C, // [rd, rs, imm16] - rd = rs >> imm16 (arithmetic)
		CLZ = 0x3D, // [rd, rs] - rd = leading zero bits in rs; 32 when rs is zero
		POPCNT = 0x3E, // [rd, rs] - rd = set bits in rs
		BSWAP = 0x3F, // [rd, rs] - rd = rs with its four bytes reversed

		// Memory Access //
		MOV = 0x40, // [rd, rs] - rd = rs
		FMOV = 0x41, // [fd, fs] - fd = fs (floating-point move)
		LI = 0x42, // [rd, imm16] - rd = imm16
		LUI = 0x43, // [rd, imm16] - rd = (imm16 << 16)
		LDR = 0x48, // [rd, rs, imm16] - rd = *(u32*)(rs + imm16)
		LDRB = 0x44, // [rd, rs, imm16] - rd = *(u8*)(rs + imm16) (load byte)
		LDRH = 0x45, // [rd, rs, imm16] - rd = *(u16*)(rs + imm16) (load halfword)
		LDRSB = 0x46, // [rd, rs, imm16] - rd = *(i8*)(rs + imm16) (load signed byte)
		LDRSH = 0x47, // [rd, rs, imm16] - rd = *(i16*)(rs + imm16) (load signed halfword)
		FLDR = 0x49, // [fd, rs, imm16] - fd = *(float*)(rs + imm16) (floating-point load)
		STR = 0x4A, // [rd, rs, imm16] - *(u32*)(rd + imm16) = rs. Base in Rd and source in Rs: Rt overlaps imm16.
		STRB = 0x4B, // [rd, rs, imm16] - *(u8*)(rd + imm16) = rs
		STRH = 0x4C, // [rd, rs, imm16] - *(u16*)(rd + imm16) = rs
		FSTR = 0x4D, // [rd, fs, imm16] - *(float*)(rd + imm16) = fs (floating-point store)
		LEA = 0x4E, // [rd, rs, imm16] - rd = rs + imm16 (load effective address)

		// Control Flow //
		JP = 0x50, // [simm24] - PC = (PC + simm24): Jump to the instruction at the given relative address.
		JPR = 0x51, // [rs] - PC = rs: Jump to the instruction at the address contained in the given register.
		CMP = 0x52, // [rs, rt] - Compare the values in rs and rt, setting the zero, sign and carry flags accordingly.
		CMPI = 0x53, // [rs, simm16] - Compare the value in rs with the immediate value imm16, setting the zero, sign and carry flags accordingly.
		FCMP = 0x54, // [fs, ft] - Compare the values in fs and ft, setting the zero, sign and carry flags accordingly (floating-point compare).
		JZ = 0x55, // [simm24] - PC = (PC + simm24): Jump to the instruction at the given relative address if the zero flag is set.
		JZR = 0x56, // [rs] - PC = rs: Jump to the instruction at the address contained in the given register if the zero flag is set.
		JNZ = 0x57, // [simm24] - PC = (PC + simm24): Jump to the instruction at the given relative address if the zero flag is not set.
		JNZR = 0x58, // [rs] - PC = rs: Jump to the instruction at the address contained in the given register if the zero flag is not set.
		JC = 0x59, // [simm24] - PC = (PC + simm24): Jump to the instruction at the given relative address if the carry flag is set.
		JCR = 0x5A, // [rs] - PC = rs: Jump to the instruction at the address contained in the given register if the carry flag is set.
		JNC = 0x5B, // [simm24] - PC = (PC + simm24): Jump to the instruction at the given relative address if the carry flag is not set.
		JNCR = 0x5C, // [rs] - PC = rs: Jump to the instruction at the address contained in the given register if the carry flag is not set.
		JS = 0x5D, // [simm24] - PC = (PC + simm24): Jump to the instruction at the given relative address if the sign flag is set.
		JSR = 0x5E, // [rs] - PC = rs: Jump to the instruction at the address contained in the given register if the sign flag is set.
		JNS = 0x5F, // [simm24] - PC = (PC + simm24): Jump to the instruction at the given relative address if the sign flag is not set.
		JNSR = 0x60, // [rs] - PC = rs: Jump to the instruction at the address contained in the given register if the sign flag is not set.
		CALL = 0x61, // [simm24] - Call the function at the given address, pushing the return address onto the call stack.
		CALLR = 0x62, // [rs] - Call the function at the address contained in the given register, pushing the return address onto the call
		RET = 0x63, // [] - Return from the current function, popping the return address from the call stack and jumping to it.
		JO = 0x64, // [simm24] - PC = (PC + simm24): Jump if the overflow flag is set.
		JOR = 0x65, // [rs] - PC = rs: Jump if the overflow flag is set.
		JNO = 0x66, // [simm24] - PC = (PC + simm24): Jump if the overflow flag is not set.
		JNOR = 0x67, // [rs] - PC = rs: Jump if the overflow flag is not set.

		// Comparison jumps. CMP leaves Zero, Sign, Carry and Overflow set, but no single-flag jump
		// spells "greater" or "less or equal": signed ordering needs Sign against Overflow and
		// unsigned ordering needs Carry against Zero. These read two flags each so a comparison
		// costs one instruction instead of a three-word dance around the existing jumps.
		// Signed - what `i32` means by <, <=, > and >=.
		JGR = 0x68, // [simm24] - Jump if greater (signed): !Zero && Sign == Overflow.
		JGRR = 0x69, // [rs] - Register form of JGR.
		JGE = 0x6A, // [simm24] - Jump if greater or equal (signed): Sign == Overflow.
		JGER = 0x6B, // [rs] - Register form of JGE.
		JLS = 0x6C, // [simm24] - Jump if less (signed): Sign != Overflow.
		JLSR = 0x6D, // [rs] - Register form of JLS.
		JLE = 0x6E, // [simm24] - Jump if less or equal (signed): Zero || Sign != Overflow.
		JLER = 0x6F, // [rs] - Register form of JLE.
		// Unsigned - "above" and "below", the same orderings over u32.
		JAB = 0x70, // [simm24] - Jump if above (unsigned): !Carry && !Zero.
		JABR = 0x71, // [rs] - Register form of JAB.
		JAE = 0x72, // [simm24] - Jump if above or equal (unsigned): !Carry.
		JAER = 0x73, // [rs] - Register form of JAE.
		JBL = 0x74, // [simm24] - Jump if below (unsigned): Carry.
		JBLR = 0x75, // [rs] - Register form of JBL.
		JBE = 0x76, // [simm24] - Jump if below or equal (unsigned): Carry || Zero.
		JBER = 0x77, // [rs] - Register form of JBE.

		// Branch and link: the return address goes into a register instead of onto the stack, so a
		// leaf costs no memory traffic and a tail call is a jump. The link register is an operand
		// rather than a fixed one, which is why nothing here is reserved and CALL/RET are unchanged
		// - a function that wants the stack discipline keeps it.
		BL = 0x78, // [rd, simm20] - rd = pc + 4; pc += simm20. Rd costs the displacement four bits.
		BLR = 0x79, // [rd, rs] - rd = pc + 4; pc = rs

		// Stack Operations //
		PUSH = 0x80, // [rs] - Push the value of the given register onto the stack.
		POP = 0x81, // [rd] - Pop the value from the stack into the given register.
		PUSHF = 0x82, // [] - Push the value of the given register onto the stack frame.
		POPF = 0x83, // [] - Pop the value from the stack frame into the given register.
		FPUSH = 0x84, // [fs] - Push the value of the given floating-point register onto the stack.
		FPOP = 0x85, // [fd] - Pop the value from the stack into the given floating-point register.
		// One bit per register, which is what imm16 has exactly sixteen of. PUSHM stores from the
		// highest set bit down, so the lowest-numbered register ends up at the lowest address and
		// POPM walks back up in order: a matching pair restores what it saved whatever the mask.
		PUSHM = 0x86, // [imm16] - Push every register whose bit is set, r15 first.
		POPM = 0x87, // [imm16] - Pop into every register whose bit is set, r0 first.
		// A whole prologue and epilogue. ENTER was three instructions and twelve bytes at the top
		// of every function that had a frame; it is the one sequence common enough to be worth an
		// opcode, and the one place fp is written at all.
		ENTER = 0x88, // [imm16] - push fp; fp = sp; sp -= imm16
		LEAVE = 0x89, // [] - sp = fp; pop fp

		// Conversions //
		ITOF = 0x90, // [fd, rs] - Convert the integer value in rs to a floating-point value and store it in fd.
		IITOF = 0x91, // [fd, rs] - Convert the signed integer value in rs to a floating-point value and store it in fd.
		FTOI = 0x92, // [rd, fs] - Convert the floating-point value in fs to an integer value and store it in rd.
		FTOII = 0x93, // [rd, fs] - Convert the floating-point value in fs to a signed integer value and store it in rd.
		MTF = 0x94, // [fd, rs] - Move the bit pattern of the integer value in rs to the floating-point register fd without conversion.
		MFF = 0x95, // [rd, fs] - Move the bit pattern of the floating-point value in fs to the integer register rd without conversion.
		FSQRT = 0x96, // [fd, fs] - fd = sqrt(fs)
		FABS = 0x97, // [fd, fs] - fd = |fs|

		// Extended float arithmetic: the primitives a software math library needs for range
		// reduction and polynomial evaluation (sin/cos/log/exp/pow), none of which get an opcode
		// of their own - see docs/05-Instruction-Set.md for why.
		FMOD = 0x98, // [fd, fs, ft] - fd = IEEE remainder of fs / ft; traps on ft == 0, like FDIV
		FMIN = 0x99, // [fd, fs, ft] - fd = min(fs, ft)
		FMAX = 0x9A, // [fd, fs, ft] - fd = max(fs, ft)
		FROUND = 0x9B, // [fd, fs] - fd = fs rounded to the nearest integer, ties to even
		FFLOOR = 0x9C, // [fd, fs] - fd = floor(fs)
		FCEIL = 0x9D, // [fd, fs] - fd = ceil(fs)
		FTRUNC = 0x9E, // [fd, fs] - fd = fs truncated toward zero
		FCOPYSIGN = 0x9F, // [fd, fs, ft] - fd = |fs| with the sign of ft

		// I/O Operations //
		IN = 0xA0, // [rd, imm8] - Read a word from the I/O port specified by imm8 into rd.
		INB = 0xA1, // [rd, imm8] - Read a byte from the I/O port specified by imm8 into rd.
		INH = 0xA2, // [rd, imm8] - Read a halfword from the I/O port specified by imm8 into rd.
		INSB = 0xA3, // [rd, imm8] - Read a signed byte from the I/O port specified by imm8 into rd.
		INSH = 0xA4, // [rd, imm8] - Read a signed halfword from the I/O port specified by imm8 into rd.
		INM = 0xA5, // [rd, rs, imm8] - Read an array of bytes with size specified by rs from the I/O port specified by imm8 into the memory address pointed to by rd.
		INR = 0xA6, // [rd, rs] - Read a word from the I/O port specified by the value in rs into rd.
		INRB = 0xA7, // [rd, rs] - Read a byte from the I/O port specified by the value in rs into rd.
		INRH = 0xA8, // [rd, rs] - Read a halfword from the I/O port specified by the value in rs into rd.
		INRSB = 0xA9, // [rd, rs] - Read a signed byte from the I/O port specified by the value in rs into rd.
		INRSH = 0xAA, // [rd, rs] - Read a signed halfword from the I/O port specified by the value in rs into rd.
		INRM = 0xAB, // [rd, rs, rt] - Read an array of bytes with size specified by rt from the I/O port specified by the value in rs into the memory address pointed to by rd.
		OUT = 0xAC, // [rs, imm8] - Write a word from rs to the I/O port specified by imm8.
		OUTB = 0xAD, // [rs, imm8] - Write a byte from rs to the I/O port specified by imm8.
		OUTH = 0xAE, // [rs, imm8] - Write a halfword from rs to the I/O port specified by imm8.
		OUTM = 0xAF, // [rs, rt, imm8] - Write an array of bytes with size specified by rt from the memory address pointed to by rs to the I/O port specified by imm8. Assembly operand order is (port, address, size), matching INM.
		OUTR = 0xB0, // [rs, rt] - Write a word from rs to the I/O port specified by the value in rt.
		OUTRB = 0xB1, // [rs, rt] - Write a byte from rs to the I/O port specified by the value in rt.
		OUTRH = 0xB2, // [rs, rt] - Write a halfword from rs to the I/O port specified by the value in rt.
		OUTRM = 0xB3, // [rd, rs, rt] - Write an array of bytes with size specified by rd from the memory address pointed to by rs to the I/O port specified by the value in rt.

		// Indexed addressing: the offset is a register instead of a displacement, so walking an
		// array costs no ADD per element. They are here rather than beside 0x40-0x4E because only
		// one slot was left there, and ten consecutive numbers say more than ten scattered ones.
		LDRX = 0xB4, // [rd, rs, rt] - rd = *(u32*)(rs + rt)
		LDRBX = 0xB5, // [rd, rs, rt] - rd = *(u8*)(rs + rt)
		LDRHX = 0xB6, // [rd, rs, rt] - rd = *(u16*)(rs + rt)
		LDRSBX = 0xB7, // [rd, rs, rt] - rd = *(i8*)(rs + rt)
		LDRSHX = 0xB8, // [rd, rs, rt] - rd = *(i16*)(rs + rt)
		FLDRX = 0xB9, // [fd, rs, rt] - fd = *(float*)(rs + rt)
		// Base in Rd and value in Rs, the same way the displacement stores are shaped: the value
		// register cannot move, because Rt is now the index rather than a spare field.
		STRX = 0xBA, // [rd, rs, rt] - *(u32*)(rd + rt) = rs
		STRBX = 0xBB, // [rd, rs, rt] - *(u8*)(rd + rt) = rs
		STRHX = 0xBC, // [rd, rs, rt] - *(u16*)(rd + rt) = rs
		FSTRX = 0xBD, // [rd, fs, rt] - *(float*)(rd + rt) = fs

		// PC-relative access to a static, in one word. The displacement is measured from the
		// instruction itself, exactly as a branch's is, so nothing has to materialise a 32-bit
		// address first and no register is borrowed to hold one. Reach is +/- 32 KiB.
		LDRP = 0xBE, // [rd, simm16] - rd = *(u32*)(pc + simm16)
		LDRBP = 0xBF, // [rd, simm16] - rd = *(u8*)(pc + simm16)
		LDRHP = 0xC0, // [rd, simm16] - rd = *(u16*)(pc + simm16)
		LDRSBP = 0xC1, // [rd, simm16] - rd = *(i8*)(pc + simm16)
		LDRSHP = 0xC2, // [rd, simm16] - rd = *(i16*)(pc + simm16)
		FLDRP = 0xC3, // [fd, simm16] - fd = *(float*)(pc + simm16)
		STRP = 0xC4, // [rs, simm16] - *(u32*)(pc + simm16) = rs
		STRBP = 0xC5, // [rs, simm16] - *(u8*)(pc + simm16) = rs
		STRHP = 0xC6, // [rs, simm16] - *(u16*)(pc + simm16) = rs
		FSTRP = 0xC7, // [fs, simm16] - *(float*)(pc + simm16) = fs

		// Rotations, and the two widenings a load from memory does for free but a register move
		// could not do at all. Shift amounts use the low five bits, like SHL and friends.
		ROL = 0xC8, // [rd, rs, rt] - rd = rs rotated left by rt
		ROLI = 0xC9, // [rd, rs, imm16] - rd = rs rotated left by imm16
		ROR = 0xCA, // [rd, rs, rt] - rd = rs rotated right by rt
		RORI = 0xCB, // [rd, rs, imm16] - rd = rs rotated right by imm16
		SXTB = 0xCC, // [rd, rs] - rd = the low byte of rs, sign-extended
		SXTH = 0xCD, // [rd, rs] - rd = the low halfword of rs, sign-extended
		MINI = 0xCE, // [rd, rs, imm16] - rd = min(rs, imm16) (unsigned)
		IMINI = 0xCF, // [rd, rs, simm16] - rd = min(rs, simm16) (signed)
		MAXI = 0xD0, // [rd, rs, imm16] - rd = max(rs, imm16) (unsigned)
		IMAXI = 0xD1, // [rd, rs, simm16] - rd = max(rs, simm16) (signed)

		// More extended float arithmetic, plus the integer complement CLZ was always missing.
		FMA = 0xD2, // [fd, fs, ft] - fd = fd + fs * ft, rounded once (fd is read as the accumulator)
		FCLASS = 0xD3, // [rd, fs] - rd = bitmask classifying fs (NaN/infinity/zero/subnormal/normal, see docs)
		FRECIPE = 0xD4, // [fd, fs] - fd = 1 / fs; traps on fs == 0, like FDIV
		FRSQRTE = 0xD5, // [fd, fs] - fd = 1 / sqrt(fs); traps on fs == 0
		CTZ = 0xD6, // [rd, rs] - rd = trailing zero bits in rs; 32 when rs is zero

		// Free: 0x08-0x0F, 0x4F, 0x7A-0x7F, 0x8A-0x8F, 0xD7-0xFF.
		// Miscellaneous - Reserved //
	};
}

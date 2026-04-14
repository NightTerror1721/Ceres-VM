#pragma once

#include "common/types.h"

namespace ceres::vm
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
		IMULI = 0x1D, // [rd, rs, imm16] - rd = rs * imm16 (signed)
		FMUL = 0x1E, // [fd, fs, ft] - fd = fs * ft (floating-point multiply)
		DIV = 0x1F, // [rd, rs, rt] - rd = rs / rt
		DIVI = 0x20, // [rd, rs, imm16] - rd = rs / imm16
		IDIV = 0x21, // [rd, rs, rt] - rd = rs / rt (signed)
		IDIVI = 0x22, // [rd, rs, imm16] - rd = rs / imm16 (signed)
		FDIV = 0x23, // [fd, fs, ft] - fd = fs / ft (floating-point divide)
		MOD = 0x24, // [rd, rs, rt] - rd = rs % rt
		MODI = 0x25, // [rd, rs, imm16] - rd = rs % imm16
		IMOD = 0x26, // [rd, rs, rt] - rd = rs % rt (signed)
		IMODI = 0x27, // [rd, rs, imm16] - rd = rs % imm16 (signed)
		FNEG = 0x28, // [fd, fs] - fd = -fs (floating-point negate)

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

		// Memory Access //
		MOV = 0x40, // [rd, rs] - rd = rs
		MOVI = 0x41, // [rd, imm16] - rd = imm16
		MOVIH = 0x42, // [rd, imm16] - rd = (imm16 << 16)
		FMOV = 0x43, // [fd, fs] - fd = fs (floating-point move)
		LDRB = 0x44, // [rd, rs, imm16] - rd = *(u8*)(rs + imm16)
		ILDRB = 0x45, // [rd, rs, imm16] - rd = *(i8*)(rs + imm16)
		LDRW = 0x46, // [rd, rs, imm16] - rd = *(u16*)(rs + imm16)
		ILDRW = 0x47, // [rd, rs, imm16] - rd = *(i16*)(rs + imm16)
		LDRD = 0x48, // [rd, rs, imm16] - rd = *(u32*)(rs + imm16)
		FLDR = 0x49, // [fd, rs, imm16] - fd = *(float*)(rs + imm16) (floating-point load)
		LEA = 0x4A, // [rd, rs, imm16] - rd = rs + imm16 (load effective address)
		STRB = 0x4B, // [rs, rt, imm16] - *(u8*)(rs + imm16) = rt
		STRW = 0x4C, // [rs, rt, imm16] - *(u16*)(rs + imm16) = rt
		STRD = 0x4D, // [rs, rt, imm16] - *(u32*)(rs + imm16) = rt
		FSTR = 0x4E, // [rs, fd, imm16] - *(float*)(rs + imm16) = fd (floating-point store)

		// Control Flow //
		JP = 0x50, // [simm24] - PC = (PC + simm24): Jump to the instruction at the given relative address.
		JPR = 0x51, // [rs] - PC = rs: Jump to the instruction at the address contained in the given register.
		CMP = 0x52, // [rs, rt] - Compare the values in rs and rt, setting the zero, sign and carry flags accordingly.
		CMPI = 0x53, // [rs, imm16] - Compare the value in rs with the immediate value imm16, setting the zero, sign and carry flags accordingly.
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

		// Stack Operations //
		PUSH = 0x70, // [rs] - Push the value of the given register onto the stack.
		POP = 0x71, // [rd] - Pop the value from the stack into the given register.
		PUSHF = 0x72, // [rs] - Push the value of the given register onto the stack frame.
		POPF = 0x73, // [rd] - Pop the value from the stack frame into the given register.
		FPUSH = 0x74, // [fs] - Push the value of the given floating-point register onto the stack.
		FPOP = 0x75, // [fd] - Pop the value from the stack into the given floating-point register.

		// Conversions //
		ITOF = 0x80, // [fd, rs] - Convert the integer value in rs to a floating-point value and store it in fd.
		IITOF = 0x81, // [fd, rs] - Convert the signed integer value in rs to a floating-point value and store it in fd.
		FTOI = 0x82, // [rd, fs] - Convert the floating-point value in fs to an integer value and store it in rd.
		FTOII = 0x83, // [rd, fs] - Convert the floating-point value in fs to a signed integer value and store it in rd.
		MTF = 0x84, // [fd, rs] - Move the bit pattern of the integer value in rs to the floating-point register fd without conversion.
		MFF = 0x85, // [rd, fs] - Move the bit pattern of the floating-point value in fs to the integer register rd without conversion.

		// I/O Operations //
		IN = 0x90, // [rd, imm8] - Read a byte from the I/O port specified by imm8 into rd.
		INR = 0x91, // [rd, rs] - Read a byte from the I/O port specified by the value in rs into rd.
		OUT = 0x92, // [rs, imm8] - Write a byte from rs to the I/O port specified by imm8.
		OUTR = 0x93, // [rs, rt] - Write a byte from rs to the I/O port specified by the value in rt.

		// Miscellaneous - Reserved //
	};
}

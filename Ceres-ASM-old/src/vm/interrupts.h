#pragma once

#include "address.h"

namespace ceres::vm
{
	// Interrupt numbers 0-15 are reserved for system use (e.g., exceptions, traps, etc.)
	enum class InterruptNumber : u8
	{
		// System Exceptions (0-15)
		Reset = 0, // Reset exception, triggered on system reset
		Trap = 1, // Trap exception, triggered by the TRAP instruction or other trap conditions
		IllegalInstruction = 2, // Illegal instruction exception, triggered when an invalid opcode is encountered
		MemoryFault = 3, // Memory fault exception, triggered on invalid memory access (e.g., out-of-bounds access, null pointer dereference, etc.)
		DivisionByZero = 4, // Division by zero exception, triggered when a division by zero is attempted
		StackOverflow = 5, // Stack overflow exception, triggered when the stack pointer exceeds memory limits
		AlignmentFault = 6, // Alignment fault exception, triggered when an unaligned memory access is attempted
		
		Syscall = 15, // System call interrupt, triggered by the SYSCALL instruction (if implemented)

		// User-defined interrupts (16-63)
		UserInterrupt0 = 16,
		UserInterrupt1 = 17,
		UserInterrupt2 = 18,
		UserInterrupt3 = 19,
		UserInterrupt4 = 20,
		UserInterrupt5 = 21,
		UserInterrupt6 = 22,
		UserInterrupt7 = 23,
		UserInterrupt8 = 24,
		UserInterrupt9 = 25,
		UserInterrupt10 = 26,
		UserInterrupt11 = 27,
		UserInterrupt12 = 28,
		UserInterrupt13 = 29,
		UserInterrupt14 = 30,
		UserInterrupt15 = 31,
		UserInterrupt16 = 32,
		UserInterrupt17 = 33,
		UserInterrupt18 = 34,
		UserInterrupt19 = 35,
		UserInterrupt20 = 36,
		UserInterrupt21 = 37,
		UserInterrupt22 = 38,
		UserInterrupt23 = 39,
		UserInterrupt24 = 40,
		UserInterrupt25 = 41,
		UserInterrupt26 = 42,
		UserInterrupt27 = 43,
		UserInterrupt28 = 44,
		UserInterrupt29 = 45,
		UserInterrupt30 = 46,
		UserInterrupt31 = 47,
		UserInterrupt32 = 48,
		UserInterrupt33 = 49,
		UserInterrupt34 = 50,
		UserInterrupt35 = 51,
		UserInterrupt36 = 52,
		UserInterrupt37 = 53,
		UserInterrupt38 = 54,
		UserInterrupt39 = 55,
		UserInterrupt40 = 56,
		UserInterrupt41 = 57,
		UserInterrupt42 = 58,
		UserInterrupt43 = 59,
		UserInterrupt44 = 60,
		UserInterrupt45 = 61,
		UserInterrupt46 = 62,
		UserInterrupt47 = 63,
	};

	inline constexpr usize InterruptNumberCount = 64;
	inline constexpr usize ReservedInterruptCount = 16; // Interrupt numbers 0-15 are reserved for system use
}

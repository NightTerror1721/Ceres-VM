#pragma once

#include "common/types.h"

namespace ceres::casm
{
	enum class SectionType : u8
	{
		Text, // Code section containing executable instructions.
		Rodata, // Read-only data section containing immutable data like string literals and constant values.
		Data, // Data section containing initialized mutable data.
		BSS, // BSS (Block Started by Symbol) section containing uninitialized mutable data that should be zero-initialized at runtime.
	};

	enum class DataType : u8
	{
		U8, // Unsigned 8-bit integer data type.
		U16, // Unsigned 16-bit integer data type.
		U32, // Unsigned 32-bit integer data type.
		String, // String data type.
	};

	enum class KeywordType : u8
	{
		Let, // 'let' keyword for defining variables.
		Constant, // 'const' keyword for defining constants.
		Global, // 'global' keyword for defining global symbols.
	};

	enum class Mnemonic : u8
	{
		NOP, // NOP
		HALT, // HALT
		TRAP, // TRAP
		RESET, // RESET
		INT, // INT
		IRET, // IRET

		ADD, // ADD, ADDI
		ADC, // ADDC, ADDCI
		SUB, // SUB, SUBI
		SBC, // SUBC, SUBCI
		MUL, // MUL, MULI
		IMUL, // IMUL, IMULI
		DIV, // DIV, DIVI
		IDIV, // IDIV, IDIVI
		MOD, // MOD, MODI
		IMOD, // IMOD, IMODI

		AND, // AND, ANDI
		OR, // OR, ORI
		XOR, // XOR, XORI
		NOT, // NOT
		SHL, // SHL, SHLI
		SHR, // SHR, SHRI
		SAR, // SAR, SARI

		MOV, // MOV, MOVI, LD
		MOVH,
		MOVB,
		MOVW,
		LEA,

		JP,
		CMP,
		JZ,
		JNZ,
		JC,
		JNC,
		JS,
		JNS,
		CALL,
		RET,

		PUSH,
		POP,
		PUSHF,
		POPF,

		IN,
		OUT
	};
}

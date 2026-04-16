#pragma once

#include "common/types.h"
#include "identifier.h"
#include <string>
#include <optional>

namespace ceres::casm
{
	using LiteralIntegerType = u32; // Using u32 for integer literals, can be adjusted as needed
	using LiteralFloatType = f32; // Using float for floating-point literals
	using LiteralCharType = u8; // Using u8 for character literals, can be adjusted as needed
	using LiteralBoolType = bool; // Using bool for boolean literals
	using LiteralStringType = std::string; // Using std::string for string literals, can be adjusted as needed

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
		I8, // Signed 8-bit integer data type.
		I16, // Signed 16-bit integer data type.
		I32, // Signed 32-bit integer data type.
		F32, // 32-bit floating-point data type.
		Char, // Character data type (Alias for u8).
		Bool, // Boolean data type (Alias for u8, where 0 represents false and any non-zero value represents true).
		String, // String data type (Alias for a null-terminated u8[]). Used for defining string literals and string variables.
	};

	enum class KeywordType : u8
	{
		Let, // 'let' keyword for defining variables.
		Constant, // 'const' keyword for defining constants.
		Global, // 'global' keyword for defining global symbols.
	};

	enum class LabelLevel : u8
	{
		Global, // Global label level, accessible from any scope.
		File, // File-level label level, accessible within the same source file.
		Local, // Local label level, accessible only within the current global or file-level label scope.
	};

	forceinline constexpr bool isIntegerDataType(DataType type) noexcept
	{
		switch (type)
		{
			case DataType::U8:
			case DataType::U16:
			case DataType::U32:
			case DataType::I8:
			case DataType::I16:
			case DataType::I32:
				return true;
			default:
				return false;
		}
	}

	forceinline constexpr bool isScalarDataType(DataType type) noexcept
	{
		switch (type)
		{
			case DataType::U8:
			case DataType::U16:
			case DataType::U32:
			case DataType::I8:
			case DataType::I16:
			case DataType::I32:
			case DataType::F32:
			case DataType::Char:
			case DataType::Bool:
				return true;
			default:
				return false;
		}
	}

	inline constexpr std::string_view dataTypeToString(DataType type) noexcept
	{
		switch (type)
		{
			case DataType::U8: return "u8";
			case DataType::U16: return "u16";
			case DataType::U32: return "u32";
			case DataType::I8: return "i8";
			case DataType::I16: return "i16";
			case DataType::I32: return "i32";
			case DataType::F32: return "f32";
			case DataType::Char: return "char";
			case DataType::Bool: return "bool";
			case DataType::String: return "string";
			default: return "unknown";
		}
	}

	forceinline constexpr u32 dataTypeSizeInBytes(DataType type) noexcept
	{
		switch (type)
		{
			case DataType::U8:
			case DataType::I8:
			case DataType::Char:
			case DataType::Bool:
				return 1;
			case DataType::U16:
			case DataType::I16:
				return 2;
			case DataType::U32:
			case DataType::I32:
			case DataType::F32:
				return 4;
			default:
				return 0; // Unknown or unsupported data type
		}
	}
}

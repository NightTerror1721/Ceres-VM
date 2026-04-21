#pragma once

#include "common/types.h"
#include "identifier.h"
#include <string>
#include <optional>

namespace ceres::casm
{
	enum class SectionType : u8
	{
		Text, // Code section containing executable instructions.
		Rodata, // Read-only data section containing immutable data like string literals and constant values.
		Data, // Data section containing initialized mutable data.
		BSS, // BSS (Block Started by Symbol) section containing uninitialized mutable data that should be zero-initialized at runtime.
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
}

#pragma once

#include <ceres/core/base/types.h>
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
		Import, // 'import' keyword for importing modules or libraries.
		Macro, // 'macro' keyword for defining macros.
		EndMacro, // 'endmacro' keyword for ending macro definitions.
		Alias, // 'alias' keyword for naming a register.
		Struct, // 'struct' keyword for declaring a record layout.
		EndStruct, // 'endstruct' keyword for closing one.
		Align, // 'align' directive: pad the current section up to a boundary.
		Org, // 'org' directive: pad the current section up to an offset within it.
		Assert, // 'assert' directive: a constant expression that has to hold at assembly time.
	};

	enum class LabelLevel : u8
	{
		Global, // Global label level, accessible from any scope.
		File, // File-level label level, accessible within the same source file.
		Local, // Local label level, accessible only within the current global or file-level label scope.
	};
}

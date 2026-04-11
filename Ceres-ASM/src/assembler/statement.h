#pragma once

#include "operand.h"
#include <optional>

namespace ceres::casm
{
	struct SectionStatement
	{
		SectionType section; // Section type (e.g., @text, @data, @rodata, @bss)
	};

	struct LabelStatement
	{
		std::string name; // Label name (e.g., "main", "loop_start", etc.)
		bool isLocal; // Whether the label is local (starts with a dot) or global
	};

	struct DataStatement
	{
		KeywordType keyword; // Keyword type (let, const, global)
		std::string identifier; // Identifier name (e.g., variable name)
		std::optional<DataType> dataType; // Optional data type (u8, u16, u32, string)
		std::variant<std::monostate, u32, std::string> value; // Initial value for the variable/constant (can be an immediate value or a string literal)

		constexpr bool hasDataType() const noexcept { return dataType.has_value(); }
		constexpr bool hasValue() const noexcept { return !std::holds_alternative<std::monostate>(value); }

		constexpr bool isIntegerValue() const noexcept { return std::holds_alternative<u32>(value); }
		constexpr bool isStringValue() const noexcept { return std::holds_alternative<std::string>(value); }

		constexpr u32 integerValue() const noexcept { return std::get<u32>(value); }
		constexpr std::string_view stringValue() const noexcept { return std::get<std::string>(value); }
	};
}

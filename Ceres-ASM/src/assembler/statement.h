#pragma once

#include "operand.h"
#include "literal_value.h"
#include "datatype_info.h"
#include "mnemonic.h"
#include <optional>
#include <vector>

namespace ceres::casm
{
	struct SectionStatement
	{
		SectionType section; // Section type (e.g., @text, @data, @rodata, @bss)
	};

	struct LabelStatement
	{
		Identifier name; // Label name (e.g., "main", "loop_start", etc.)
		bool isLocal; // Whether the label is local (starts with a dot) or global
		bool isGlobal; // Whether the label is global (defined with 'global' keyword) or not
	};

	struct DataStatement
	{
		bool isConstant; // Whether the data is a constant (defined with 'const') or a variable (defined with 'let')
		Identifier identifier; // Identifier name (e.g., variable name)
		std::optional<DataTypeInfo> dataType; // Optional data type information (can be scalar, unsized array, or sized array)
		std::optional<LiteralValue> value; // Optional initial value (can be a literal integer, float, char, bool, string, or an array of literal values)
	};

	struct InstructionStatement
	{
		Mnemonic mnemonic; // Instruction mnemonic (e.g., ADD, SUB, etc.)
		std::vector<Operand> operands; // Operands for the instruction (can be registers, immediates, memory operands, etc.)
	};

	class Statement
	{
	private:
		std::variant<SectionStatement, LabelStatement, DataStatement, InstructionStatement> _value;

	public:
		Statement() noexcept = default;
		Statement(const Statement&) noexcept = default;
		Statement(Statement&&) noexcept = default;
		~Statement() noexcept = default;

		Statement& operator=(const Statement&) noexcept = default;
		Statement& operator=(Statement&&) noexcept = default;

	private:
		 Statement(std::variant<SectionStatement, LabelStatement, DataStatement, InstructionStatement>&& value) noexcept
			: _value(std::move(value))
		 {}

	public:
		constexpr bool isSection() const noexcept { return std::holds_alternative<SectionStatement>(_value); }
		constexpr bool isLabel() const noexcept { return std::holds_alternative<LabelStatement>(_value); }
		constexpr bool isData() const noexcept { return std::holds_alternative<DataStatement>(_value); }
		constexpr bool isInstruction() const noexcept { return std::holds_alternative<InstructionStatement>(_value); }

		constexpr const SectionStatement& asSection() const noexcept { return std::get<SectionStatement>(_value); }
		constexpr const LabelStatement& asLabel() const noexcept { return std::get<LabelStatement>(_value); }
		constexpr const DataStatement& asData() const noexcept { return std::get<DataStatement>(_value); }
		constexpr const InstructionStatement& asInstruction() const noexcept { return std::get<InstructionStatement>(_value); }

	public:
		static Statement makeSection(SectionType section) noexcept
		{
			return Statement{ SectionStatement{ section } };
		}

		static Statement makeLabel(const Identifier& name, bool isLocal, bool isGlobal) noexcept
		{
			return Statement{ LabelStatement{ name, isLocal, isGlobal } };
		}
		static Statement makeLabel(Identifier&& name, bool isLocal, bool isGlobal) noexcept
		{
			return Statement{ LabelStatement{ std::move(name), isLocal, isGlobal } };
		}

		static Statement makeData(bool isConstant, const Identifier& identifier, std::optional<DataTypeInfo>&& dataType = std::nullopt) noexcept
		{
			return Statement{ DataStatement{ isConstant, identifier, dataType, std::nullopt } };
		}

		static Statement makeData(bool isConstant, const Identifier& identifier, std::optional<DataTypeInfo>&& dataType, std::optional<LiteralValue>&& value) noexcept
		{
			return Statement{ DataStatement{ isConstant, identifier, std::move(dataType), std::move(value) } };
		}

		static Statement makeInstruction(Mnemonic mnemonic, std::vector<Operand>&& operands) noexcept
		{
			return Statement{ InstructionStatement{ mnemonic, std::move(operands) } };
		}
	};
}

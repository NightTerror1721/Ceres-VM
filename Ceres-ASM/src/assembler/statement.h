#pragma once

#include "operand.h"
#include "literal_value.h"
#include "mnemonic.h"
#include <optional>
#include <vector>
#include <array>

namespace ceres::casm
{
	struct SectionStatement
	{
		SectionType section; // Section type (e.g., @text, @data, @rodata, @bss)
	};

	struct LabelStatement
	{
		Identifier name; // Label name (e.g., "main", "loop_start", etc.)
		LabelLevel level; // Label level (global, file-level, or local)
	};

	struct DataStatement
	{
		bool isConstant; // Whether the data is a constant (defined with 'const') or a variable (defined with 'let')
		Identifier identifier; // Identifier name (e.g., variable name)
		std::optional<DataTypeReference> dataType; // Optional data type information (can be scalar, unsized array, or sized array)
		std::optional<LiteralValueReference> value; // Optional initial value (can be a literal integer, float, char, bool, string, or an array of literal values)
	};

	struct InstructionStatement
	{
		Mnemonic mnemonic; // Instruction mnemonic (e.g., ADD, SUB, etc.)
		std::vector<Operand> operands; // Operands for the instruction (can be registers, immediates, memory operands, etc.)

		InstructionSignature signature() const noexcept
		{
			InstructionSignature::OperandArray operandArray{};
			for (usize i = 0; i < operands.size() && i < InstructionSignature::MaxOperandsPerInstruction; ++i)
			{
				operandArray[i] = operands[i].type();
			}
			return InstructionSignature{ mnemonic, operandArray };
		}
	};

	class Statement
	{
	private:
		u32 _line = 0; // Line number in the source code where the statement is located
		std::variant<SectionStatement, LabelStatement, DataStatement, InstructionStatement> _value;

	public:
		Statement() noexcept = default;
		Statement(const Statement&) noexcept = default;
		Statement(Statement&&) noexcept = default;
		~Statement() noexcept = default;

		Statement& operator=(const Statement&) noexcept = default;
		Statement& operator=(Statement&&) noexcept = default;

	private:
		 explicit Statement(u32 line, std::variant<SectionStatement, LabelStatement, DataStatement, InstructionStatement>&& value) noexcept :
			 _line(line),
			_value(std::move(value))
		 {}

	public:
		constexpr u32 line() const noexcept { return _line; }

		constexpr bool isSection() const noexcept { return std::holds_alternative<SectionStatement>(_value); }
		constexpr bool isLabel() const noexcept { return std::holds_alternative<LabelStatement>(_value); }
		constexpr bool isData() const noexcept { return std::holds_alternative<DataStatement>(_value); }
		constexpr bool isInstruction() const noexcept { return std::holds_alternative<InstructionStatement>(_value); }

		constexpr const SectionStatement& asSection() const noexcept { return std::get<SectionStatement>(_value); }
		constexpr const LabelStatement& asLabel() const noexcept { return std::get<LabelStatement>(_value); }
		constexpr const DataStatement& asData() const noexcept { return std::get<DataStatement>(_value); }
		constexpr const InstructionStatement& asInstruction() const noexcept { return std::get<InstructionStatement>(_value); }

		constexpr SectionStatement& asSection() noexcept { return std::get<SectionStatement>(_value); }
		constexpr LabelStatement& asLabel() noexcept { return std::get<LabelStatement>(_value); }
		constexpr DataStatement& asData() noexcept { return std::get<DataStatement>(_value); }
		constexpr InstructionStatement& asInstruction() noexcept { return std::get<InstructionStatement>(_value); }

	public:
		static Statement makeSection(u32 line, SectionType section) noexcept	
		{
			return Statement{ line, SectionStatement{ section } };
		}

		static Statement makeLabel(u32 line, const Identifier& name, LabelLevel level) noexcept
		{
			return Statement{ line, LabelStatement{ name, level } };
		}
		static Statement makeLabel(u32 line, Identifier&& name, LabelLevel level) noexcept
		{
			return Statement{ line, LabelStatement{ std::move(name), level } };
		}

		static Statement makeData(u32 line, bool isConstant, const Identifier& identifier, std::optional<DataTypeReference>&& dataType = std::nullopt) noexcept
		{
			return Statement{ line, DataStatement{ isConstant, identifier, dataType, std::nullopt } };
		}

		static Statement makeData(u32 line, bool isConstant, const Identifier& identifier, std::optional<DataTypeReference>&& dataType, std::optional<LiteralValueReference>&& value) noexcept
		{
			return Statement{ line, DataStatement{ isConstant, identifier, std::move(dataType), std::move(value) } };
		}

		static Statement makeInstruction(u32 line, Mnemonic mnemonic, std::vector<Operand>&& operands) noexcept
		{
			return Statement{ line, InstructionStatement{ mnemonic, std::move(operands) } };
		}
	};
}

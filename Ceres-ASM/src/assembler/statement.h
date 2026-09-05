#pragma once

#include "operand.h"
#include "literal_value.h"
#include "mnemonic.h"
#include "strings_pool.h"
#include <optional>
#include <vector>
#include <array>

namespace ceres::casm
{
	class Statement;

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
		Identifier name; // Identifier name (e.g., variable name)
		DataTypeReference dataType = DataTypeReference::Invalid; // Data type of the variable (e.g., u8, u16, u32, string, etc.)
		LiteralValueReference value = LiteralValueReference::makeEmpty(); // Optional initial value (can be a literal integer, float, char, bool, string, or an array of literal values)
	};

	struct ImportStatement
	{
		LiteralString moduleName; // Name of the module to import (e.g., "math", "utils", etc.)
	};

	struct MacroDeclarationStatement
	{
		Identifier name; // Name of the macro being declared
		FixedVector<Identifier> parameters; // List of parameter names for the macro
		FixedVector<Statement> body; // List of statements that make up the macro's body
	};

	struct MacroLabelStatement
	{
		Identifier name; // Name of the macro label being defined
	};

	struct MacroCallStatement
	{
		Identifier name; // Name of the macro being called
		FixedVector<Operand> arguments; // List of arguments passed to the macro

		constexpr usize arity() const noexcept { return arguments.size(); }
	};

	struct InstructionStatement
	{
		Mnemonic mnemonic; // Instruction mnemonic (e.g., ADD, SUB, etc.)
		FixedVector<Operand> operands; // Operands for the instruction (can be registers, immediates, memory operands, etc.)

		InstructionSignature signature() const noexcept
		{
			InstructionSignature::OperandArray operandArray{};
			for (usize i = 0; i < operands.size() && i < InstructionSignature::MaxOperandsPerInstruction; ++i)
				operandArray[i] = operands[i].type();
			return InstructionSignature{ mnemonic, operandArray };
		}
	};

	class Statement
	{
	private:
		using StatementVariant = std::variant<
			SectionStatement,
			LabelStatement,
			DataStatement,
			ImportStatement,
			MacroDeclarationStatement,
			MacroLabelStatement,
			MacroCallStatement,
			InstructionStatement
		>;

	private:
		u32 _line = 0; // Line number in the source code where the statement is located
		StatementVariant _value;

	public:
		Statement() noexcept = default;
		Statement(const Statement&) noexcept = default;
		Statement(Statement&&) noexcept = default;
		~Statement() noexcept = default;

		Statement& operator=(const Statement&) noexcept = default;
		Statement& operator=(Statement&&) noexcept = default;

	private:
		 explicit Statement(u32 line, StatementVariant&& value) noexcept :
			 _line(line),
			_value(std::move(value))
		 {}

	public:
		constexpr u32 line() const noexcept { return _line; }

		constexpr bool isSection() const noexcept { return std::holds_alternative<SectionStatement>(_value); }
		constexpr bool isLabel() const noexcept { return std::holds_alternative<LabelStatement>(_value); }
		constexpr bool isData() const noexcept { return std::holds_alternative<DataStatement>(_value); }
		constexpr bool isImport() const noexcept { return std::holds_alternative<ImportStatement>(_value); }
		constexpr bool isMacroDeclaration() const noexcept { return std::holds_alternative<MacroDeclarationStatement>(_value); }
		constexpr bool isMacroLabel() const noexcept { return std::holds_alternative<MacroLabelStatement>(_value); }
		constexpr bool isMacroCall() const noexcept { return std::holds_alternative<MacroCallStatement>(_value); }
		constexpr bool isInstruction() const noexcept { return std::holds_alternative<InstructionStatement>(_value); }

		constexpr const SectionStatement& asSection() const noexcept { return std::get<SectionStatement>(_value); }
		constexpr const LabelStatement& asLabel() const noexcept { return std::get<LabelStatement>(_value); }
		constexpr const DataStatement& asData() const noexcept { return std::get<DataStatement>(_value); }
		constexpr const ImportStatement& asImport() const noexcept { return std::get<ImportStatement>(_value); }
		constexpr const MacroDeclarationStatement& asMacroDeclaration() const noexcept { return std::get<MacroDeclarationStatement>(_value); }
		constexpr const MacroLabelStatement& asMacroLabel() const noexcept { return std::get<MacroLabelStatement>(_value); }
		constexpr const MacroCallStatement& asMacroCall() const noexcept { return std::get<MacroCallStatement>(_value); }
		constexpr const InstructionStatement& asInstruction() const noexcept { return std::get<InstructionStatement>(_value); }

		constexpr SectionStatement& asSection() noexcept { return std::get<SectionStatement>(_value); }
		constexpr LabelStatement& asLabel() noexcept { return std::get<LabelStatement>(_value); }
		constexpr DataStatement& asData() noexcept { return std::get<DataStatement>(_value); }
		constexpr ImportStatement& asImport() noexcept { return std::get<ImportStatement>(_value); }
		constexpr MacroDeclarationStatement& asMacroDeclaration() noexcept { return std::get<MacroDeclarationStatement>(_value); }
		constexpr MacroLabelStatement& asMacroLabel() noexcept { return std::get<MacroLabelStatement>(_value); }
		constexpr MacroCallStatement& asMacroCall() noexcept { return std::get<MacroCallStatement>(_value); }
		constexpr InstructionStatement& asInstruction() noexcept { return std::get<InstructionStatement>(_value); }

	public:
		static Statement makeSection(u32 line, SectionType section) noexcept	
		{
			return Statement{ line, SectionStatement{ section } };
		}

		static Statement makeLabel(u32 line, Identifier name, LabelLevel level) noexcept
		{
			return Statement{ line, LabelStatement{ name, level } };
		}

		static Statement makeData(u32 line, bool isConstant, Identifier identifier, DataTypeReference dataType = DataTypeReference::Invalid) noexcept
		{
			return Statement{ line, DataStatement{ isConstant, identifier, dataType, LiteralValueReference::makeEmpty() } };
		}

		static Statement makeData(u32 line, bool isConstant, Identifier identifier, DataTypeReference dataType, LiteralValueReference value = LiteralValueReference::makeEmpty()) noexcept
		{
			return Statement{ line, DataStatement{ isConstant, identifier, dataType, value } };
		}

		static Statement makeImport(u32 line, LiteralString moduleName) noexcept
		{
			return Statement{ line, ImportStatement{ moduleName } };
		}

		static Statement makeMacroDeclaration(u32 line, Identifier name, std::vector<Identifier>&& parameters, std::vector<Statement>&& body) noexcept
		{
			return Statement{ line, MacroDeclarationStatement{ name, std::move(parameters), std::move(body) } };
		}

		static Statement makeMacroLabel(u32 line, Identifier name) noexcept
		{
			return Statement{ line, MacroLabelStatement{ name } };
		}

		static Statement makeMacroCall(u32 line, Identifier name, std::vector<Operand>&& arguments) noexcept
		{
			return Statement{ line, MacroCallStatement{ name, std::move(arguments) } };
		}

		static Statement makeInstruction(u32 line, Mnemonic mnemonic, std::vector<Operand>&& operands) noexcept
		{
			return Statement{ line, InstructionStatement{ mnemonic, std::move(operands) } };
		}
	};
}

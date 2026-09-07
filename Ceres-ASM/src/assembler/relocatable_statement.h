#pragma once

#include "statement.h"
#include "vm/address.h"
#include "vm/instructions.h"

namespace ceres::casm
{
	struct ResolvedDataStatement
	{
		bool isConstant; // Whether the data is a constant (defined with 'const') or a variable (defined with 'let')
		bool isGlobal; // Declared with the 'global' prefix
		Identifier name; // Identifier name (e.g., variable name)
		DataType dataType; // Resolved data type information (can be scalar, unsized array, or sized array)
		std::optional<LiteralValue> value; // Optional initial value (can be a literal integer, float, char, bool, string, or an array of literal values)
	};

	class RelocatableStatement
	{
	public:
		using RelocatableStatementVariant = std::variant<
			SectionStatement,
			LabelStatement,
			ResolvedDataStatement,
			InstructionStatement
		>;

	private:
		std::string_view _file; // View into AssemblyState's interned path storage (see Statement::_file)
		u32 _line; // Line number in the source code where the statement is located
		// Carried through from Statement: where the user's own code is, as opposed to where the
		// instruction was written. See Statement::_expansionFile.
		std::string_view _expansionFile;
		u32 _expansionLine = 0;
		u16 _macroDepth = 0; // 0 for a statement written by hand
		u32 _size = 0; // Size of the statement in bytes (set during assembly)
		std::optional<vm::Address> _address; // Address in program memory where the statement will be located (set during assembly)
		RelocatableStatementVariant _value;

	public:
		RelocatableStatement() noexcept = default;
		RelocatableStatement(const RelocatableStatement&) noexcept = default;
		RelocatableStatement(RelocatableStatement&&) noexcept = default;
		~RelocatableStatement() noexcept = default;

		RelocatableStatement& operator=(const RelocatableStatement&) noexcept = default;
		RelocatableStatement& operator=(RelocatableStatement&&) noexcept = default;

	private:
		explicit RelocatableStatement(
			std::string_view file,
			u32 line,
			u32 size,
			std::optional<vm::Address> address,
			RelocatableStatementVariant&& value
		) noexcept :
			_file(file),
			_line(line),
			_size(size),
			_address(address),
			_value(std::move(value))
		{}

	public:
		constexpr std::string_view file() const noexcept { return _file; }
		constexpr u32 line() const noexcept { return _line; }

		// Empty/zero means the statement was never macro-expanded, in which case the expansion
		// site *is* the statement's own site. Resolving that here keeps every caller from having
		// to remember the special case.
		constexpr std::string_view expansionFile() const noexcept { return _expansionFile.empty() ? _file : _expansionFile; }
		constexpr u32 expansionLine() const noexcept { return _expansionLine == 0 ? _line : _expansionLine; }
		constexpr u16 macroDepth() const noexcept { return _macroDepth; }

		constexpr void setExpansionSite(std::string_view file, u32 line, u16 macroDepth) noexcept
		{
			_expansionFile = file;
			_expansionLine = line;
			_macroDepth = macroDepth;
		}

		constexpr bool hasAddress() const noexcept { return _address.has_value(); }
		constexpr vm::Address address() const noexcept { return _address.value(); }

		constexpr u32 size() const noexcept { return _size; }

		constexpr bool isSection() const noexcept { return std::holds_alternative<SectionStatement>(_value); }
		constexpr bool isLabel() const noexcept { return std::holds_alternative<LabelStatement>(_value); }
		constexpr bool isData() const noexcept { return std::holds_alternative<ResolvedDataStatement>(_value); }
		constexpr bool isInstruction() const noexcept { return std::holds_alternative<InstructionStatement>(_value); }

		const SectionStatement& asSection() const noexcept { return std::get<SectionStatement>(_value); }
		const LabelStatement& asLabel() const noexcept { return std::get<LabelStatement>(_value); }
		const ResolvedDataStatement& asData() const noexcept { return std::get<ResolvedDataStatement>(_value); }
		const InstructionStatement& asInstruction() const noexcept { return std::get<InstructionStatement>(_value); }

		InstructionStatement& asInstruction() noexcept { return std::get<InstructionStatement>(_value); }

		void setAddress(vm::Address address) noexcept { _address = address; }

	public:
		static RelocatableStatement makeSection(std::string_view file, u32 line, SectionStatement&& section) noexcept
		{
			return RelocatableStatement(file, line, 0, std::nullopt, std::move(section));
		}

		static RelocatableStatement makeLabel(std::string_view file, u32 line, vm::Address address, LabelStatement&& label) noexcept
		{
			return RelocatableStatement(file, line, 0, address, std::move(label));
		}

		static RelocatableStatement makeData(std::string_view file, u32 line, u32 size, ResolvedDataStatement&& data) noexcept
		{
			return RelocatableStatement(file, line, size, std::nullopt, std::move(data));
		}
		static RelocatableStatement makeData(std::string_view file, u32 line, u32 size, vm::Address address, ResolvedDataStatement&& data) noexcept
		{
			return RelocatableStatement(file, line, size, address, std::move(data));
		}

		static RelocatableStatement makeInstruction(std::string_view file, u32 line, vm::Address address, InstructionStatement&& instruction) noexcept
		{
			return RelocatableStatement(file, line, vm::Instruction::Size, address, std::move(instruction));
		}
	};
}

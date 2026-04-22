#pragma once

#include "statement.h"
#include "vm/address.h"
#include "vm/instructions.h"

namespace ceres::casm
{
	struct ResolvedDataStatement
	{
		bool isConstant; // Whether the data is a constant (defined with 'const') or a variable (defined with 'let')
		std::string name; // Identifier name (e.g., variable name)
		DataType dataType; // Resolved data type information (can be scalar, unsized array, or sized array)
		std::optional<LiteralValue> value; // Optional initial value (can be a literal integer, float, char, bool, string, or an array of literal values)
	};

	class RelocatableStatement
	{
	private:
		u32 _line; // Line number in the source code where the statement is located
		u32 _size = 0; // Size of the statement in bytes (set during assembly)
		std::optional<vm::Address> _address; // Address in program memory where the statement will be located (set during assembly)
		std::variant<SectionStatement, LabelStatement, ResolvedDataStatement, InstructionStatement> _value;

	public:
		RelocatableStatement() noexcept = default;
		RelocatableStatement(const RelocatableStatement&) noexcept = default;
		RelocatableStatement(RelocatableStatement&&) noexcept = default;
		~RelocatableStatement() noexcept = default;

		RelocatableStatement& operator=(const RelocatableStatement&) noexcept = default;
		RelocatableStatement& operator=(RelocatableStatement&&) noexcept = default;

	private:
		explicit RelocatableStatement(
			u32 line,
			u32 size,
			std::optional<vm::Address> address,
			std::variant<SectionStatement, LabelStatement, ResolvedDataStatement, InstructionStatement>&& value
		) noexcept :
			_line(line),
			_size(size),
			_address(address),
			_value(std::move(value))
		{}

	public:
		constexpr u32 line() const noexcept { return _line; }

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
		static RelocatableStatement makeSection(u32 line, SectionStatement&& section) noexcept
		{
			return RelocatableStatement(line, 0, std::nullopt, std::move(section));
		}

		static RelocatableStatement makeLabel(u32 line, vm::Address address, LabelStatement&& label) noexcept
		{
			return RelocatableStatement(line, 0, address, std::move(label));
		}

		static RelocatableStatement makeData(u32 line, u32 size, ResolvedDataStatement&& data) noexcept
		{
			return RelocatableStatement(line, size, std::nullopt, std::move(data));
		}
		static RelocatableStatement makeData(u32 line, u32 size, vm::Address address, ResolvedDataStatement&& data) noexcept
		{
			return RelocatableStatement(line, size, address, std::move(data));
		}

		static RelocatableStatement makeInstruction(u32 line, vm::Address address, InstructionStatement&& instruction) noexcept
		{
			return RelocatableStatement(line, vm::Instruction::Size, address, std::move(instruction));
		}
	};
}

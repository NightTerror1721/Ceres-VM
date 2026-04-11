#pragma once

#include "common_defs.h"
#include <variant>
#include <string>

namespace ceres::casm
{
	struct RegisterOperand
	{
		u8 regIndex; // Register index (0-15)
	};

	struct ImmediateOperand
	{
		u32 value; // Immediate value (32-bit unsigned integer)
	};

	struct IdentifierOperand
	{
		std::string name; // Identifier name (e.g., variable name, label name, etc.)
	};

	struct MemoryOperand
	{
		u8 baseRegIndex; // Base register index (0-15)
		// Offset can be either a register, an immediate value, or an identifier (e.g., for symbolic addresses)
		std::variant<std::monostate, RegisterOperand, ImmediateOperand, IdentifierOperand> offset;

		constexpr bool hasOffset() const noexcept { return !std::holds_alternative<std::monostate>(offset); }
		constexpr bool isRegisterOffset() const noexcept { return std::holds_alternative<RegisterOperand>(offset); }
		constexpr bool isImmediateOffset() const noexcept { return std::holds_alternative<ImmediateOperand>(offset); }
		constexpr bool isIdentifierOffset() const noexcept { return std::holds_alternative<IdentifierOperand>(offset); }

		constexpr const RegisterOperand& registerOffset() const noexcept { return std::get<RegisterOperand>(offset); }
		constexpr const ImmediateOperand& immediateOffset() const noexcept { return std::get<ImmediateOperand>(offset); }
		constexpr const IdentifierOperand& identifierOffset() const noexcept { return std::get<IdentifierOperand>(offset); }
	};

	class Operand
	{
	private:
		std::variant<std::monostate, RegisterOperand, ImmediateOperand, IdentifierOperand, MemoryOperand> _value;

	public:
		Operand() noexcept = default;
		Operand(const Operand&) noexcept = default;
		Operand(Operand&&) noexcept = default;
		~Operand() noexcept = default;

		Operand& operator=(const Operand&) noexcept = default;
		Operand& operator=(Operand&&) noexcept = default;

		bool operator==(const Operand& other) const noexcept = default;

	private:
		 Operand(std::variant<std::monostate, RegisterOperand, ImmediateOperand, IdentifierOperand, MemoryOperand>&& value) noexcept
			: _value(std::move(value))
		 {}

	public:
		constexpr bool isValid() const noexcept { return !_value.valueless_by_exception() && !std::holds_alternative<std::monostate>(_value); }
		constexpr bool isRegister() const noexcept { return std::holds_alternative<RegisterOperand>(_value); }
		constexpr bool isImmediate() const noexcept { return std::holds_alternative<ImmediateOperand>(_value); }
		constexpr bool isIdentifier() const noexcept { return std::holds_alternative<IdentifierOperand>(_value); }
		constexpr bool isMemory() const noexcept { return std::holds_alternative<MemoryOperand>(_value); }

		constexpr const RegisterOperand& asRegister() const noexcept { return std::get<RegisterOperand>(_value); }
		constexpr const ImmediateOperand& asImmediate() const noexcept { return std::get<ImmediateOperand>(_value); }
		constexpr const IdentifierOperand& asIdentifier() const noexcept { return std::get<IdentifierOperand>(_value); }
		constexpr const MemoryOperand& asMemory() const noexcept { return std::get<MemoryOperand>(_value); }

	public:
		constexpr explicit operator bool() const noexcept { return isValid(); }
		constexpr bool operator!() const noexcept { return !isValid(); }

	public:
		static Operand makeRegister(u8 regIndex) noexcept { return Operand{ RegisterOperand{ regIndex } }; }
		static Operand makeImmediate(u32 value) noexcept { return Operand{ ImmediateOperand{ value } }; }
		static Operand makeIdentifier(std::string_view name) noexcept { return Operand{ IdentifierOperand{ std::string(name) } }; }
		static Operand makeMemory(u8 baseRegIndex) noexcept { return Operand{ MemoryOperand{ baseRegIndex, std::monostate{} } }; }
		static Operand makeMemory(u8 baseRegIndex, u8 registerOffset) noexcept { return Operand{ MemoryOperand{ baseRegIndex, RegisterOperand{ registerOffset } } }; }
		static Operand makeMemory(u8 baseRegIndex, u32 immediateOffset) noexcept { return Operand{ MemoryOperand{ baseRegIndex, ImmediateOperand{ immediateOffset } } }; }
		static Operand makeMemory(u8 baseRegIndex, std::string_view identifierOffset) noexcept
		{
			return Operand{ MemoryOperand{ baseRegIndex, IdentifierOperand{ std::string(identifierOffset) } } };
		}
	};
}

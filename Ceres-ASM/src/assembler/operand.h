#pragma once

#include "common_defs.h"
#include "literal_value.h"
#include "instruction_info.h"
#include <variant>
#include <string>
#include <expected>

namespace ceres::casm
{
	struct RegisterOperand
	{
		u8 regIndex; // Register index (0-15)
	};

	struct FloatingPointRegisterOperand
	{
		u8 regIndex; // Floating-point register index (0-15)
	};

	struct ImmediateOperand
	{
		u32 value; // Immediate value (32-bit unsigned integer)
	};

	struct IdentifierOperand
	{
		std::string name; // Identifier name (e.g., variable name, label name, etc.)
		bool isLocal; // Whether the identifier is a local label (e.g., .label) or a global label, variable, constant (e.g., label, variable, constant)
	};

	struct MemoryOperand
	{
		u8 baseRegIndex; // Base register index (0-15)
		// Offset can be either an immediate value, or an identifier (e.g., for symbolic addresses)
		std::variant<std::monostate, ImmediateOperand, IdentifierOperand> offset;

		constexpr bool hasOffset() const noexcept { return !std::holds_alternative<std::monostate>(offset); }
		constexpr bool isImmediateOffset() const noexcept { return std::holds_alternative<ImmediateOperand>(offset); }
		constexpr bool isIdentifierOffset() const noexcept { return std::holds_alternative<IdentifierOperand>(offset); }

		constexpr const ImmediateOperand& immediateOffset() const noexcept { return std::get<ImmediateOperand>(offset); }
		constexpr const IdentifierOperand& identifierOffset() const noexcept { return std::get<IdentifierOperand>(offset); }
	};

	struct VariableOperand
	{
		DataTypeScalarCode scalarCode; // Scalar code for the variable (e.g., U8, S8, U16, S16, U32, S32, F32)
		vm::Address address; // Address of the variable in memory
	};

	struct LabelOperand
	{
		vm::Address address; // Address of the label in memory
	};

	class Operand
	{
	public:
		using OperandVariant = std::variant<std::monostate, RegisterOperand, FloatingPointRegisterOperand, ImmediateOperand, IdentifierOperand, MemoryOperand, VariableOperand, LabelOperand>;

	private:
		OperandVariant _value;

	public:
		Operand() noexcept = default;
		Operand(const Operand&) noexcept = default;
		Operand(Operand&&) noexcept = default;
		~Operand() noexcept = default;

		Operand& operator=(const Operand&) noexcept = default;
		Operand& operator=(Operand&&) noexcept = default;

		bool operator==(const Operand&) const noexcept = default;

	private:
		 Operand(OperandVariant&& value) noexcept
			: _value(std::move(value))
		 {}

	public:
		constexpr bool isValid() const noexcept { return !_value.valueless_by_exception() && !std::holds_alternative<std::monostate>(_value); }
		constexpr bool isRegister() const noexcept { return std::holds_alternative<RegisterOperand>(_value); }
		constexpr bool isFloatingPointRegister() const noexcept { return std::holds_alternative<FloatingPointRegisterOperand>(_value); }
		constexpr bool isImmediate() const noexcept { return std::holds_alternative<ImmediateOperand>(_value); }
		constexpr bool isIdentifier() const noexcept { return std::holds_alternative<IdentifierOperand>(_value); }
		constexpr bool isMemory() const noexcept { return std::holds_alternative<MemoryOperand>(_value); }
		constexpr bool isVariable() const noexcept { return std::holds_alternative<VariableOperand>(_value); }
		constexpr bool isLabel() const noexcept { return std::holds_alternative<LabelOperand>(_value); }

		constexpr const RegisterOperand& asRegister() const noexcept { return std::get<RegisterOperand>(_value); }
		constexpr const FloatingPointRegisterOperand& asFloatingPointRegister() const noexcept { return std::get<FloatingPointRegisterOperand>(_value); }
		constexpr const ImmediateOperand& asImmediate() const noexcept { return std::get<ImmediateOperand>(_value); }
		constexpr const IdentifierOperand& asIdentifier() const noexcept { return std::get<IdentifierOperand>(_value); }
		constexpr const MemoryOperand& asMemory() const noexcept { return std::get<MemoryOperand>(_value); }
		constexpr const VariableOperand& asVariable() const noexcept { return std::get<VariableOperand>(_value); }
		constexpr const LabelOperand& asLabel() const noexcept { return std::get<LabelOperand>(_value); }

		constexpr OperandType type() const noexcept
		{
			if (isRegister())
				return OperandType::IntegralRegister;
			else if (isFloatingPointRegister())
				return OperandType::FloatingPointRegister;
			else if (isImmediate())
				return OperandType::Immediate;
			else if (isIdentifier())
				return OperandType::Invalid; // Identifiers are not directly valid operand types; they need to be resolved to a value
			else if (isMemory())
				return OperandType::RegisterPlusAddress;
			else if (isVariable())
			{
				const auto& var = asVariable();
				switch (var.scalarCode)
				{
					case DataTypeScalarCode::U8: return OperandType::VariableU8;
					case DataTypeScalarCode::I8: return OperandType::VariableS8;
					case DataTypeScalarCode::U16: return OperandType::VariableU16;
					case DataTypeScalarCode::I16: return OperandType::VariableS16;
					case DataTypeScalarCode::U32: return OperandType::VariableU32;
					case DataTypeScalarCode::I32: return OperandType::VariableS32;
					case DataTypeScalarCode::F32: return OperandType::VariableF32;
					default: return OperandType::Invalid;
				}
			}
			else if (isLabel())
				return OperandType::Label;
			else
				return OperandType::Invalid;
		}

	public:
		constexpr explicit operator bool() const noexcept { return isValid(); }
		constexpr bool operator!() const noexcept { return !isValid(); }

	public:
		static Operand makeRegister(u8 regIndex) noexcept { return Operand{ RegisterOperand{ regIndex } }; }
		static Operand makeFloatingPointRegister(u8 regIndex) noexcept { return Operand{ FloatingPointRegisterOperand{ regIndex } }; }
		static Operand makeImmediate(u32 value) noexcept { return Operand{ ImmediateOperand{ value } }; }
		static Operand makeIdentifier(std::string_view name, bool isLocal) noexcept { return Operand{ IdentifierOperand{ std::string(name), isLocal } }; }
		static Operand makeIdentifier(std::string&& name, bool isLocal) noexcept { return Operand{ IdentifierOperand{ std::move(name), isLocal } }; }
		static Operand makeMemory(u8 baseRegIndex) noexcept { return Operand{ MemoryOperand{ baseRegIndex, std::monostate{} } }; }
		static Operand makeMemory(u8 baseRegIndex, u32 immediateOffset) noexcept { return Operand{ MemoryOperand{ baseRegIndex, ImmediateOperand{ immediateOffset } } }; }
		static Operand makeMemory(u8 baseRegIndex, std::string_view identifierOffset) noexcept
		{
			return Operand{ MemoryOperand{ baseRegIndex, IdentifierOperand{ std::string(identifierOffset) } } };
		}
		static Operand makeMemory(u8 baseRegIndex, std::string&& identifierOffset) noexcept
		{
			return Operand{ MemoryOperand{ baseRegIndex, IdentifierOperand{ std::move(identifierOffset) } } };
		}
		static Operand makeVariable(DataTypeScalarCode scalarCode, vm::Address address) noexcept
		{
			return Operand{ VariableOperand{ scalarCode, address } };
		}
		static Operand makeLabel(vm::Address address) noexcept
		{
			return Operand{ LabelOperand{ address } };
		}

	public:
		static std::expected<Operand, std::string_view> makeFromLiteralValue(const LiteralValue& value) noexcept;
	};

	struct RegisterInfo
	{
		u8 index; // Register index (0-15)
		bool isFloatingPoint; // Whether the register is a floating-point register

		static std::optional<RegisterInfo> get(std::string_view name) noexcept;
	};
}

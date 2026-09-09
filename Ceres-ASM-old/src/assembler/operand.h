#pragma once

#include "common_defs.h"
#include "literal_value.h"
#include "instruction_info.h"
#include "strings_pool.h"
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
		Identifier name; // Identifier name (e.g., variable name, label name, etc.)
		bool isLocal; // Whether the identifier is a local label (e.g., .label) or a global label, variable, constant (e.g., label, variable, constant)
		// Written inside brackets: `[counter]`. The bracket means what it means everywhere else in
		// the language - the contents of that address, not the address itself - so this is what
		// keeps `mov r1, counter` and `mov r1, [counter]` from being the same operand.
		bool dereferenced = false;
	};

	struct MacroParameterOperand
	{
		Identifier name; // Macro parameter name (e.g., "$param1", "$param2", etc.)
	};

	struct MemoryOperand
	{
		// Absent when the base is a symbol rather than a register: `[counter]`. The symbol's own
		// address is the base then, and the assembler builds the access out of it.
		u8 baseRegIndex; // Base register index (0-15)
		// The width and signedness of the access, when it was written down: `u8[r2 + 4]`. Absent
		// for a plain `[r2 + 4]`, where the mnemonic carries the width instead. The encoding is the
		// same either way - this only picks which opcode the signature resolves to.
		std::optional<DataTypeScalarCode> accessType;
		// An immediate, an identifier (a symbolic address), or a second register - `[r1 + r2]`,
		// which is an index rather than a displacement and picks a different opcode. Inside a macro
		// body it can also be a parameter, and then which of those three it is depends on the
		// argument.
		std::variant<std::monostate, ImmediateOperand, IdentifierOperand, RegisterOperand, MacroParameterOperand> offset;

		// `[$base + 4]` inside a macro body. Which register the base is has not been decided yet:
		// the argument decides it, at expansion. Null everywhere else, which is everywhere the
		// base is already a register.
		NullableIdentifier baseParameter{};

		constexpr bool hasParameterBase() const noexcept { return !baseParameter.isNull(); }
		constexpr bool isParameterOffset() const noexcept { return std::holds_alternative<MacroParameterOperand>(offset); }
		constexpr const MacroParameterOperand& parameterOffset() const noexcept { return std::get<MacroParameterOperand>(offset); }

		constexpr bool hasOffset() const noexcept { return !std::holds_alternative<std::monostate>(offset); }
		constexpr bool isImmediateOffset() const noexcept { return std::holds_alternative<ImmediateOperand>(offset); }
		constexpr bool isIdentifierOffset() const noexcept { return std::holds_alternative<IdentifierOperand>(offset); }
		constexpr bool isRegisterOffset() const noexcept { return std::holds_alternative<RegisterOperand>(offset); }

		constexpr const ImmediateOperand& immediateOffset() const noexcept { return std::get<ImmediateOperand>(offset); }
		constexpr const IdentifierOperand& identifierOffset() const noexcept { return std::get<IdentifierOperand>(offset); }
		constexpr const RegisterOperand& registerOffset() const noexcept { return std::get<RegisterOperand>(offset); }
	};

	struct VariableOperand
	{
		DataTypeScalarCode scalarCode; // Scalar code for the variable (e.g., U8, S8, U16, S16, U32, S32, F32)
		vm::Address address; // Address of the variable in memory
		bool dereferenced = false; // Written as `[name]`: the contents, not the address
		// Which symbol the address came from, kept past the point where it was resolved. A build
		// that links everything at once has no use for it - the address is the whole answer - but a
		// unit assembled on its own has to say what the address was *of*, because its sections have
		// not been placed and the symbol may not even be defined here.
		NullableIdentifier symbol{};
		SectionType section = SectionType::Text;
		// Defined somewhere other than the unit being assembled: only a link knows where.
		bool external = false;
	};

	struct LabelOperand
	{
		vm::Address address; // Address of the label in memory
		// Which symbol the address came from, kept past the point where it was resolved. A build
		// that links everything at once has no use for it - the address is the whole answer - but a
		// unit assembled on its own has to say what the address was *of*, because its sections have
		// not been placed and the symbol may not even be defined here.
		NullableIdentifier symbol{};
		SectionType section = SectionType::Text;
		// Defined somewhere other than the unit being assembled: only a link knows where.
		bool external = false;
	};

	struct MacroLabelOperand
	{
		Identifier name; // Macro label name (e.g., "%%label1", "%%label2", etc.)
	};

	// An immediate the parser could not fold on its own, because it names a constant or asks a
	// question about a symbol: `li r1, BLOCK * 2`, `li r1, sizeof(buffer)`. Replaced by a plain
	// immediate as soon as a symbol table is available (SymbolTable::resolveOperand).
	struct ConstExprOperand
	{
		ConstExpr expression;

		bool operator==(const ConstExprOperand&) const = default;
	};

	class Operand
	{
	public:
		using OperandVariant = std::variant<
			std::monostate,
			RegisterOperand,
			FloatingPointRegisterOperand,
			ImmediateOperand,
			IdentifierOperand,
			MemoryOperand,
			VariableOperand,
			LabelOperand,
			MacroParameterOperand,
			MacroLabelOperand,
			ConstExprOperand
		>;

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
		constexpr bool isMacroParameter() const noexcept { return std::holds_alternative<MacroParameterOperand>(_value); }
		constexpr bool isMacroLabel() const noexcept { return std::holds_alternative<MacroLabelOperand>(_value); }
		constexpr bool isConstExpr() const noexcept { return std::holds_alternative<ConstExprOperand>(_value); }

		constexpr const RegisterOperand& asRegister() const noexcept { return std::get<RegisterOperand>(_value); }
		constexpr const FloatingPointRegisterOperand& asFloatingPointRegister() const noexcept { return std::get<FloatingPointRegisterOperand>(_value); }
		constexpr const ImmediateOperand& asImmediate() const noexcept { return std::get<ImmediateOperand>(_value); }
		constexpr const IdentifierOperand& asIdentifier() const noexcept { return std::get<IdentifierOperand>(_value); }
		const ConstExprOperand& asConstExpr() const noexcept { return std::get<ConstExprOperand>(_value); }
		constexpr const MemoryOperand& asMemory() const noexcept { return std::get<MemoryOperand>(_value); }
		constexpr const VariableOperand& asVariable() const noexcept { return std::get<VariableOperand>(_value); }
		constexpr const LabelOperand& asLabel() const noexcept { return std::get<LabelOperand>(_value); }
		constexpr const MacroParameterOperand& asMacroParameter() const noexcept { return std::get<MacroParameterOperand>(_value); }
		constexpr const MacroLabelOperand& asMacroLabel() const noexcept { return std::get<MacroLabelOperand>(_value); }

		constexpr OperandType type() const noexcept
		{
			if (isRegister())
				return OperandType::IntegralRegister;
			else if (isFloatingPointRegister())
				return OperandType::FloatingPointRegister;
			else if (isImmediate())
				return OperandType::Immediate;
			else if (isIdentifier() || isConstExpr())
				return OperandType::Invalid; // Neither is a valid operand type until it is resolved to a value
			else if (isMemory())
			{
				const auto& memory = asMemory();
				const bool indexed = memory.isRegisterOffset();
				if (!memory.accessType.has_value())
					return indexed ? OperandType::RegisterPlusRegister : OperandType::RegisterPlusAddress;

				switch (memory.accessType.value())
				{
					case DataTypeScalarCode::U8:  return indexed ? OperandType::IndexedU8 : OperandType::MemoryU8;
					case DataTypeScalarCode::I8:  return indexed ? OperandType::IndexedS8 : OperandType::MemoryS8;
					case DataTypeScalarCode::U16: return indexed ? OperandType::IndexedU16 : OperandType::MemoryU16;
					case DataTypeScalarCode::I16: return indexed ? OperandType::IndexedS16 : OperandType::MemoryS16;
					case DataTypeScalarCode::U32: return indexed ? OperandType::IndexedU32 : OperandType::MemoryU32;
					case DataTypeScalarCode::I32: return indexed ? OperandType::IndexedS32 : OperandType::MemoryS32;
					case DataTypeScalarCode::F32: return indexed ? OperandType::IndexedF32 : OperandType::MemoryF32;
					default: return OperandType::Invalid;
				}
			}
			else if (isVariable())
			{
				const auto& var = asVariable();
				const bool deref = var.dereferenced;
				switch (var.scalarCode)
				{
					case DataTypeScalarCode::U8: return deref ? OperandType::AtVariableU8 : OperandType::VariableU8;
					case DataTypeScalarCode::I8: return deref ? OperandType::AtVariableS8 : OperandType::VariableS8;
					case DataTypeScalarCode::U16: return deref ? OperandType::AtVariableU16 : OperandType::VariableU16;
					case DataTypeScalarCode::I16: return deref ? OperandType::AtVariableS16 : OperandType::VariableS16;
					case DataTypeScalarCode::U32: return deref ? OperandType::AtVariableU32 : OperandType::VariableU32;
					case DataTypeScalarCode::I32: return deref ? OperandType::AtVariableS32 : OperandType::VariableS32;
					case DataTypeScalarCode::F32: return deref ? OperandType::AtVariableF32 : OperandType::VariableF32;
					default: return OperandType::Invalid;
				}
			}
			else if (isLabel())
				return OperandType::Label;
			else if (isMacroParameter())
				return OperandType::Invalid; // Macro parameters are not directly valid operand types; they need to be resolved to a value
			else if (isMacroLabel())
				return OperandType::Invalid; // Macro labels are not directly valid operand types; they need to be resolved to a value
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
		static Operand makeIdentifier(Identifier name, bool isLocal) noexcept { return Operand{ IdentifierOperand{ name, isLocal } }; }
		static Operand makeDereferencedIdentifier(Identifier name, bool isLocal) noexcept { return Operand{ IdentifierOperand{ name, isLocal, true } }; }
		static Operand makeMemory(u8 baseRegIndex) noexcept { return Operand{ MemoryOperand{ baseRegIndex, std::nullopt, std::monostate{} } }; }
		static Operand makeMemory(u8 baseRegIndex, u32 immediateOffset) noexcept { return Operand{ MemoryOperand{ baseRegIndex, std::nullopt, ImmediateOperand{ immediateOffset } } }; }
		static Operand makeMemoryIndexed(u8 baseRegIndex, u8 indexRegIndex) noexcept
		{
			return Operand{ MemoryOperand{ baseRegIndex, std::nullopt, RegisterOperand{ indexRegIndex } } };
		}
		// For the two callers that build one piece by piece: the parser, which meets the parts in
		// the order they are written, and macro expansion, which replaces a parameter inside one.
		static Operand makeMemoryOperand(MemoryOperand&& memory) noexcept { return Operand{ std::move(memory) }; }

		// Stamps the access type onto a memory operand that has already been parsed, which is the
		// order the parser meets them in: the type comes first but the operand is built after.
		static Operand withAccessType(Operand memory, DataTypeScalarCode scalarCode) noexcept
		{
			MemoryOperand typed = memory.asMemory();
			typed.accessType = scalarCode;
			return Operand{ std::move(typed) };
		}
		static Operand makeMemory(u8 baseRegIndex, Identifier identifierOffset) noexcept
		{
			return Operand{ MemoryOperand{ baseRegIndex, std::nullopt, IdentifierOperand{ identifierOffset } } };
		}
		static Operand makeVariable(DataTypeScalarCode scalarCode, vm::Address address, bool dereferenced = false,
			NullableIdentifier symbol = {}, SectionType section = SectionType::Data, bool external = false) noexcept
		{
			return Operand{ VariableOperand{ scalarCode, address, dereferenced, symbol, section, external } };
		}
		static Operand makeLabel(vm::Address address,
			NullableIdentifier symbol = {}, SectionType section = SectionType::Text, bool external = false) noexcept
		{
			return Operand{ LabelOperand{ address, symbol, section, external } };
		}
		static Operand makeMacroParameter(Identifier name) noexcept { return Operand{ MacroParameterOperand{ name } }; }
		static Operand makeMacroLabel(Identifier name) noexcept { return Operand{ MacroLabelOperand{ name } }; }
		static Operand makeConstExpr(ConstExpr&& expression) noexcept { return Operand{ ConstExprOperand{ std::move(expression) } }; }

	public:
		static std::expected<Operand, std::string_view> makeFromLiteralValue(const LiteralValue& value) noexcept;
	};

	struct RegisterInfo
	{
		u8 index; // Register index (0-15)
		bool isFloatingPoint; // Whether the register is a floating-point register

		static std::optional<RegisterInfo> get(Identifier name) noexcept;
	};
}

#pragma once

#include "common_defs.h"
#include "datatype_info.h"
#include "literal_value.h"
#include "operand.h"
#include "errors.h"
#include "vm/address.h"
#include <unordered_map>
#include <format>
#include <expected>

namespace ceres::casm
{
	using Address = vm::Address;

	enum class SymbolType : u8
	{
		Label,
		Constant,
		Variable
	};

	class Symbol
	{
	private:
		std::string _name;
		SymbolType _type;
		bool _global;
		bool _readonly;
		std::optional<Address> _address;
		std::optional<DataTypeInfo> _dataType;
		std::optional<LiteralValue> _value;

	public:
		Symbol() = delete;
		Symbol(const Symbol&) = delete;
		Symbol(Symbol&&) = default;
		~Symbol() = default;

		Symbol& operator=(const Symbol&) = delete;
		Symbol& operator=(Symbol&&) = default;
	private:
		explicit Symbol(
			std::string&& name,
			SymbolType type,
			bool global,
			bool readonly,
			std::optional<Address>&& address,
			std::optional<DataTypeInfo>&& dataType,
			std::optional<LiteralValue>&& value
		) noexcept :
			_name(std::move(name)),
			_type(type),
			_global(global),
			_readonly(readonly),
			_address(std::move(address)),
			_dataType(std::move(dataType)),
			_value(std::move(value))
		{}

	public:
		inline std::string_view name() const noexcept { return _name; }
		inline SymbolType type() const noexcept { return _type; }
		inline bool isGlobal() const noexcept { return _global; }
		inline bool isReadonly() const noexcept { return _readonly; }
		inline Address address() const noexcept { return _address.value(); }
		inline const DataTypeInfo& dataType() const noexcept { return _dataType.value(); }
		inline const LiteralValue& value() const noexcept { return _value.value(); }

		inline bool hasAddress() const noexcept { return _address.has_value(); }
		inline bool hasDataType() const noexcept { return _dataType.has_value(); }
		inline bool hasValue() const noexcept { return _value.has_value(); }

		inline bool isLabel() const noexcept { return _type == SymbolType::Label; }
		inline bool isConstant() const noexcept { return _type == SymbolType::Constant; }
		inline bool isVariable() const noexcept { return _type == SymbolType::Variable; }

	public:
		static Symbol makeLabel(std::string&& name, Address address, bool isGlobal) noexcept
		{
			return Symbol(std::move(name), SymbolType::Label, isGlobal, true, address, std::nullopt, std::nullopt);
		}

		static Symbol makeConstant(std::string&& name, const LiteralValue& value, bool isGlobal) noexcept
		{
			return Symbol(std::move(name), SymbolType::Constant, isGlobal, true, std::nullopt, std::nullopt, LiteralValue{ value });
		}

		static Symbol makeVariable(std::string&& name, Address address, const DataTypeInfo& dataType, std::optional<LiteralValue> value, bool isGlobal, bool isReadonly) noexcept
		{
			return Symbol(std::move(name), SymbolType::Variable, isGlobal, isReadonly, address, std::make_optional(dataType), std::move(value));
		}
	};

	class SymbolTable
	{
	private:
		std::unordered_map<std::string, Symbol> _symbols;
		std::string _lastParentLabel;

	public:
		SymbolTable() = default;
		SymbolTable(const SymbolTable&) = delete;
		SymbolTable(SymbolTable&&) = default;
		~SymbolTable() = default;

		SymbolTable& operator=(const SymbolTable&) = delete;
		SymbolTable& operator=(SymbolTable&&) = default;

	public:
		void defineLabel(u32 line, const Identifier& identifier, Address address, LabelLevel level);
		void defineConstant(u32 line, const Identifier& identifier, bool isGlobal, const LiteralValue& value);

		std::optional<std::reference_wrapper<const Symbol>> get(std::string_view name) const noexcept;
		std::optional<std::reference_wrapper<const Symbol>> getLocal(std::string_view name, const Identifier& parentIdentifier) const noexcept;

	public:
		void defineVariable(u32 line, const Identifier& identifier, Address address, bool isGlobal, bool isReadonly, const DataTypeInfo& dataType, const LiteralValue& initialValue)
		{
			defineVariable(line, identifier, address, isGlobal, isReadonly, dataType, &initialValue);
		}
		void defineVariable(u32 line, const Identifier& identifier, Address address, bool isGlobal, bool isReadonly, const DataTypeInfo& dataType)
		{
			defineVariable(line, identifier, address, isGlobal, isReadonly, dataType, nullptr);
		}

		Operand& tryResolveOperand(u32 line, Operand& operand, std::vector<std::string>& unresolvedSymbols) const
		{
			return resolveOperand(line, operand, &unresolvedSymbols);
		}
		Operand& resolveOperand(u32 line, Operand& operand) const
		{
			return resolveOperand(line, operand, nullptr);
		}

	private:
		void defineVariable(u32 line, const Identifier& identifier, Address address, bool isGlobal, bool isReadonly, const DataTypeInfo& dataType, const LiteralValue* initialValue);

		std::string resolveName(u32 line, std::string_view name, bool isLocal);

		void checkRedefinition(u32 line, const std::string& name) const;

		Operand& resolveOperand(u32 line, Operand& operand, std::vector<std::string>* unresolvedSymbols) const;

	private:
		[[noreturn]] void error(u32 line, std::string_view message) const
		{
			throw AssemblerError(line, 1, message);
		}

		template <typename... Args>
		[[noreturn]] void error(u32 line, std::string_view formatStr, Args&&... args) const
		{
			std::string message = std::vformat(formatStr, std::make_format_args(args...));
			throw AssemblerError(line, 1, message);
		}
	};
}

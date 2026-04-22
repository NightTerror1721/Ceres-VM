#pragma once

#include "common_defs.h"
#include "literal_value.h"
#include "data_type.h"
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
		SectionType _section;
		bool _global;
		bool _readonly;
		std::optional<Address> _address;
		std::optional<DataType> _dataType;
		std::optional<LiteralValue> _value;

	public:
		Symbol() = delete;
		Symbol(const Symbol&) = default;
		Symbol(Symbol&&) = default;
		~Symbol() = default;

		Symbol& operator=(const Symbol&) = default;
		Symbol& operator=(Symbol&&) = default;
	private:
		explicit Symbol(
			std::string&& name,
			SymbolType type,
			SectionType section,
			bool global,
			bool readonly,
			std::optional<Address>&& address,
			std::optional<DataType>&& dataType,
			std::optional<LiteralValue>&& value
		) noexcept :
			_name(std::move(name)),
			_type(type),
			_section(section),
			_global(global),
			_readonly(readonly),
			_address(std::move(address)),
			_dataType(std::move(dataType)),
			_value(std::move(value))
		{}

	public:
		inline std::string_view name() const noexcept { return _name; }
		inline SymbolType type() const noexcept { return _type; }
		inline SectionType section() const noexcept { return _section; }
		inline bool isGlobal() const noexcept { return _global; }
		inline bool isReadonly() const noexcept { return _readonly; }
		inline Address address() const noexcept { return _address.value(); }
		inline DataType dataType() const noexcept { return _dataType.value(); }
		inline const LiteralValue& value() const noexcept { return _value.value(); }

		inline bool hasAddress() const noexcept { return _address.has_value(); }
		inline bool hasDataType() const noexcept { return _dataType.has_value(); }
		inline bool hasValue() const noexcept { return _value.has_value(); }

		inline bool isLabel() const noexcept { return _type == SymbolType::Label; }
		inline bool isConstant() const noexcept { return _type == SymbolType::Constant; }
		inline bool isVariable() const noexcept { return _type == SymbolType::Variable; }

		inline void setAddress(Address address) noexcept { _address = address; }

	public:
		static Symbol makeLabel(std::string&& name, SectionType section, Address address, bool isGlobal) noexcept
		{
			return Symbol(std::move(name), SymbolType::Label, section, isGlobal, true, address, std::nullopt, std::nullopt);
		}

		static Symbol makeConstant(std::string&& name, const LiteralValue& value, bool isGlobal) noexcept
		{
			return Symbol(std::move(name), SymbolType::Constant, SectionType::Rodata, isGlobal, true, std::nullopt, std::nullopt, LiteralValue{ value });
		}

		static Symbol makeVariable(std::string&& name, SectionType section, Address address, DataType dataType, std::optional<LiteralValue> value, bool isGlobal, bool isReadonly) noexcept
		{
			return Symbol(std::move(name), SymbolType::Variable, section, isGlobal, isReadonly, address, std::make_optional(dataType), std::move(value));
		}
	};

	struct UnresolvedSymbol
	{
		std::string name; // Name of the unresolved symbol
		std::string parentName; // Name of the parent symbol (if applicable)
		u32 line; // Line number in the source code where the symbol is referenced

		inline bool isLocal() const noexcept { return !parentName.empty(); }
	};

	class SymbolTable
	{
	public:
		static inline constexpr std::string_view EntryPointLabelName = "main";

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
		void defineLabel(u32 line, std::string_view name, SectionType section, Address address, LabelLevel level);
		void defineConstant(u32 line, std::string_view name, bool isGlobal, const LiteralValue& value);

		std::optional<std::reference_wrapper<const Symbol>> get(std::string_view name) const noexcept;
		std::optional<std::reference_wrapper<const Symbol>> getLocal(std::string_view name, std::string_view parentName) const noexcept;

		void relocateSymbols(Address textOffset, Address dataOffset, Address rodataOffset, Address bssOffset);

	public:
		void insertRawSymbol(const Symbol& symbol)
		{
			if (const auto [it, inserted] = _symbols.emplace(symbol.name(), symbol); !inserted)
				error(0, "Redefinition of symbol: {}", it->first);
		}

		void defineVariable(u32 line, std::string_view name, SectionType section, Address address, bool isGlobal, bool isReadonly, DataType dataType, const LiteralValue& initialValue)
		{
			defineVariable(line, name, section, address, isGlobal, isReadonly, dataType, &initialValue);
		}
		void defineVariable(u32 line, std::string_view name, SectionType section, Address address, bool isGlobal, bool isReadonly, DataType dataType)
		{
			defineVariable(line, name, section, address, isGlobal, isReadonly, dataType, nullptr);
		}

		Operand& tryResolveOperand(u32 line, Operand& operand, std::string_view parentName, std::vector<UnresolvedSymbol>& unresolvedSymbols) const
		{
			return resolveOperand(line, operand, parentName, &unresolvedSymbols, nullptr);
		}
		Operand& resolveOperand(u32 line, Operand& operand, std::string_view parentName, const SymbolTable& globalSymbolTable) const
		{
			return resolveOperand(line, operand, parentName, nullptr, &globalSymbolTable);
		}

		const std::unordered_map<std::string, Symbol>& getAllSymbols() const noexcept
		{
			return _symbols;
		}

	private:
		void defineVariable(u32 line, std::string_view name, SectionType section, Address address, bool isGlobal, bool isReadonly, DataType dataType, const LiteralValue* initialValue);

		std::string resolveName(u32 line, std::string_view name, bool isLocal);

		void checkRedefinition(u32 line, const std::string& name) const;

		Operand& resolveOperand(u32 line, Operand& operand, std::string_view parentName, std::vector<UnresolvedSymbol>* unresolvedSymbols, const SymbolTable* globalSymbolTable) const;

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

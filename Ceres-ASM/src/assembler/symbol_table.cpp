#include "symbol_table.h"
#include "common/string_utils.h"

namespace ceres::casm
{
	void SymbolTable::defineLabel(u32 line, std::string_view name, SectionType section, Address address, LabelLevel level)
	{
		bool isLocal = level == LabelLevel::Local;
		std::string fullName = resolveName(line, name, isLocal);
		checkRedefinition(line, fullName);

		std::string key = fullName;
		Symbol symbol = Symbol::makeLabel(std::move(fullName), section, address, level == LabelLevel::Global);
		auto [it, inserted] = _symbols.emplace(std::move(key), std::move(symbol));
		if (!inserted)
			error(line, "Redefinition of symbol: {}", it->first);

		if (!isLocal)
			_lastParentLabel = it->first;
	}

	void SymbolTable::defineConstant(u32 line, std::string_view name, bool isGlobal, const LiteralValue& value)
	{
		std::string fullName = resolveName(line, name, false);
		checkRedefinition(line, fullName);

		std::string key = fullName;
		Symbol symbol = Symbol::makeConstant(std::move(fullName), value, isGlobal);
		auto [it, inserted] = _symbols.emplace(std::move(key), std::move(symbol));
		if (!inserted)
			error(line, "Redefinition of symbol: {}", it->first);
	}

	void SymbolTable::defineVariable(u32 line, std::string_view name, SectionType section, Address address, bool isGlobal, bool isReadonly, DataType dataType, const LiteralValue* initialValue)
	{
		std::string fullName = resolveName(line, name, false);
		checkRedefinition(line, fullName);

		std::optional<LiteralValue> value = initialValue ? std::make_optional(*initialValue) : std::nullopt;
		
		std::string key = fullName;
		Symbol symbol = Symbol::makeVariable(std::move(fullName), section, address, dataType, std::move(value), isGlobal, isReadonly);
		auto [it, inserted] = _symbols.emplace(std::move(key), std::move(symbol));
		if (!inserted)
			error(line, "Redefinition of symbol: {}", it->first);
	}

	std::optional<std::reference_wrapper<const Symbol>> SymbolTable::get(std::string_view name) const noexcept
	{
		if (const auto it = _symbols.find(std::string(name)); it != _symbols.end())
			return std::cref(it->second);
		return std::nullopt;
	}

	std::optional<std::reference_wrapper<const Symbol>> SymbolTable::getLocal(std::string_view name, std::string_view parentName) const noexcept
	{
		std::string fullName = string_utils::concat(parentName, ".", name);
		if (const auto it = _symbols.find(fullName); it != _symbols.end())
			return std::cref(it->second);
		return std::nullopt;
	}

	std::string SymbolTable::resolveName(u32 line, std::string_view name, bool isLocal)
	{
		if (!isLocal)
			return std::string(name);

		if (_lastParentLabel.empty())
			error(line, "Local label defined without a parent label");

		return string_utils::concat(_lastParentLabel, ".", name);
	}

	void SymbolTable::checkRedefinition(u32 line, const std::string& name) const
	{
		if (_symbols.contains(name))
			error(line, "Redefinition of symbol: {}", name);
	}

	Operand& SymbolTable::resolveOperand(u32 line, Operand& operand, std::string_view parentName, std::vector<UnresolvedSymbol>* unresolvedSymbols, const SymbolTable* globalSymbolTable) const
	{
		if (operand.isIdentifier())
		{
			const auto& identifierOperand = operand.asIdentifier();
			auto value = identifierOperand.isLocal && !parentName.empty()
				? getLocal(identifierOperand.name, parentName)
				: get(identifierOperand.name);

			if (!value.has_value() && globalSymbolTable != nullptr)
				value = globalSymbolTable->get(identifierOperand.name);

			if (!value.has_value())
			{
				if (unresolvedSymbols == nullptr)
					error(line, "Unresolved symbol: {}", identifierOperand.name);
				{
					UnresolvedSymbol unresolvedSymbol = {
						.name = identifierOperand.name,
						.parentName = identifierOperand.isLocal ? std::string(parentName) : std::string(),
						.line = line
					};
					unresolvedSymbols->push_back(std::move(unresolvedSymbol));
				}
			}
			else
			{
				const bool resolveConstantsOnly = unresolvedSymbols != nullptr;
				const Symbol& symbol = value.value().get();
				if (symbol.isConstant())
				{
					auto result = Operand::makeFromLiteralValue(symbol.value());
					if (!result)
						error(line, "Cannot resolve constant symbol '{}' to a valid operand. {}", symbol.name(), result.error());
					operand = std::move(result.value());
				}
				else if (symbol.isVariable() && !resolveConstantsOnly)
				{
					if (!symbol.hasAddress())
						error(line, "Variable symbol '{}' does not have a valid address", symbol.name());
					DataType dataType = symbol.dataType();
					if (!dataType.isValid())
						error(line, "Variable symbol '{}' has an invalid data type", symbol.name());
					operand = Operand::makeVariable(dataType.scalarCode(), symbol.address());
				}
				else if (symbol.isLabel() && !resolveConstantsOnly)
				{
					if (!symbol.hasAddress())
						error(line, "Label symbol '{}' does not have a valid address", symbol.name());
					operand = Operand::makeLabel(symbol.address());
				}
			}

			return operand;
		}

		if (operand.isMemory())
		{
			const auto& memoryOperand = operand.asMemory();
			if (memoryOperand.hasOffset() && memoryOperand.isIdentifierOffset())
			{
				const auto& identifierOperand = memoryOperand.identifierOffset();
				auto value = get(identifierOperand.name);
				if (!value.has_value() && globalSymbolTable != nullptr)
					value = globalSymbolTable->get(identifierOperand.name);

				if (!value.has_value())
				{
					if (unresolvedSymbols == nullptr)
						error(line, "Unresolved symbol: {}", identifierOperand.name);

					UnresolvedSymbol unresolvedSymbol = {
						.name = identifierOperand.name,
						.parentName = identifierOperand.isLocal ? std::string(parentName) : std::string(),
						.line = line
					};
					unresolvedSymbols->push_back(std::move(unresolvedSymbol));
				}
				else
				{
					const Symbol& symbol = value.value().get();
					if (!symbol.isConstant())
						error(line, "Memory operand offset must be a constant symbol, but '{}' is not a constant", symbol.name());

					auto result = Operand::makeFromLiteralValue(symbol.value());
					if (!result || !result->isImmediate())
						error(line, "Cannot resolve constant symbol '{}' to a valid immediate operand for memory offset. {}", symbol.name(), result.error());

					operand = Operand::makeMemory(memoryOperand.baseRegIndex, result.value().asImmediate().value);
				}
			}

			return operand;
		}

		return operand;
	}

	void SymbolTable::relocateSymbols(Address textOffset, Address dataOffset, Address rodataOffset, Address bssOffset)
	{
		for (auto& [name, symbol] : _symbols)
		{
			if (symbol.isLabel())
			{
				if (!symbol.hasAddress())
					error(0, "Label symbol '{}' does not have a valid address", symbol.name());

				Address newAddress = symbol.address();
				switch (symbol.section())
				{
					case SectionType::Text: newAddress += textOffset; break;
					case SectionType::Rodata: newAddress += rodataOffset; break;
					case SectionType::Data: newAddress += dataOffset; break;
					case SectionType::BSS: newAddress += bssOffset; break;
					default:
						error(0, "Label symbol '{}' has an invalid section type", symbol.name());
				}
				symbol.setAddress(newAddress);
			}
			else if (symbol.isVariable())
			{
				if (!symbol.hasAddress())
					error(0, "Variable symbol '{}' does not have a valid address", symbol.name());

				Address newAddress = symbol.address();
				switch (symbol.section())
				{
					case SectionType::Text:
						error(0, "Variable symbol '{}' cannot be in the text section", symbol.name());
						break;

					case SectionType::Rodata: newAddress += rodataOffset; break;
					case SectionType::Data: newAddress += dataOffset; break;
					case SectionType::BSS: newAddress += bssOffset; break;
					default:
						error(0, "Variable symbol '{}' has an invalid section type", symbol.name());
				}
				symbol.setAddress(newAddress);
			}
		}
	}
}

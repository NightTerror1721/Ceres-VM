#include "symbol_table.h"

namespace ceres::casm
{
	void SymbolTable::defineLabel(u32 line, const Identifier& identifier, Address address, LabelLevel level)
	{
		bool isLocal = level == LabelLevel::Local;
		std::string fullName = resolveName(line, identifier.name(), isLocal);
		checkRedefinition(line, fullName);

		std::string key = fullName;
		Symbol symbol = Symbol::makeLabel(std::move(fullName), address, level == LabelLevel::Global);
		auto [it, inserted] = _symbols.emplace(std::move(key), std::move(symbol));
		if (!inserted)
			error(line, "Redefinition of symbol: {}", it->first);

		if (!isLocal)
			_lastParentLabel = it->first;
	}

	void SymbolTable::defineConstant(u32 line, const Identifier& identifier, bool isGlobal, const LiteralValue& value)
	{
		std::string fullName = resolveName(line, identifier.name(), false);
		checkRedefinition(line, fullName);

		std::string key = fullName;
		Symbol symbol = Symbol::makeConstant(std::move(fullName), value, isGlobal);
		auto [it, inserted] = _symbols.emplace(std::move(key), std::move(symbol));
		if (!inserted)
			error(line, "Redefinition of symbol: {}", it->first);
	}

	void SymbolTable::defineVariable(u32 line, const Identifier& identifier, Address address, bool isGlobal, bool isReadonly, const DataTypeInfo& dataType, const LiteralValue* initialValue)
	{
		std::string fullName = resolveName(line, identifier.name(), false);
		checkRedefinition(line, fullName);

		std::optional<LiteralValue> value = initialValue ? std::make_optional(*initialValue) : std::nullopt;
		
		std::string key = fullName;
		Symbol symbol = Symbol::makeVariable(std::move(fullName), address, dataType, std::move(value), isGlobal, isReadonly);
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

	std::optional<std::reference_wrapper<const Symbol>> SymbolTable::getLocal(std::string_view name, const Identifier& parentIdentifier) const noexcept
	{
		std::string fullName = std::string(parentIdentifier.name()) + "." + std::string(name);
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

		return _lastParentLabel + "." + std::string(name);
	}

	void SymbolTable::checkRedefinition(u32 line, const std::string& name) const
	{
		if (_symbols.contains(name))
			error(line, "Redefinition of symbol: {}", name);
	}

	Operand& SymbolTable::resolveOperand(u32 line, Operand& operand, std::vector<std::string>* unresolvedSymbols) const
	{
		if (operand.isIdentifier())
		{
			const auto& identifierOperand = operand.asIdentifier();
			const auto& value = get(identifierOperand.identifier.name());
			if (!value.has_value())
			{
				if (unresolvedSymbols == nullptr)
					error(line, "Unresolved symbol: {}", identifierOperand.identifier.name());
				unresolvedSymbols->push_back(std::string(identifierOperand.identifier.name()));
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
					operand = Operand::makeMemory(symbol.address().value());
				}
				else if (symbol.isLabel() && !resolveConstantsOnly)
				{
					if (!symbol.hasAddress())
						error(line, "Label symbol '{}' does not have a valid address", symbol.name());
					operand = Operand::makeImmediate(symbol.address().value());
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
				const auto& value = get(identifierOperand.identifier.name());
				if (!value.has_value())
				{
					if (unresolvedSymbols == nullptr)
						error(line, "Unresolved symbol: {}", identifierOperand.identifier.name());
					unresolvedSymbols->push_back(std::string(identifierOperand.identifier.name()));
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
}

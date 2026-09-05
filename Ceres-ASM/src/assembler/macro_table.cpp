#include "macro_table.h"
#include "translation_unit.h"

namespace ceres::casm
{
	void MacroTable::defineMacro(MacroSignature&& signature, std::vector<std::string>&& parameters, std::vector<Statement>&& body)
	{
		checkRedefinition(0, signature);

		Macro macro = Macro::make(MacroSignature(signature), std::move(parameters), std::move(body));
		auto [it, inserted] = _macros.emplace(std::move(signature), std::move(macro));
		if (!inserted)
			error(0, "Macro redefinition: {}", it->first.name);

		if (it->second.parameterCount() != signature.parameterCount)
			error(0, "Macro parameter count mismatch for '{}': expected {}, got {}", it->first.name, signature.parameterCount, it->second.parameterCount());
	}

	OptionalConstRef<Macro> MacroTable::getMacro(const MacroSignature& signature) const noexcept
	{
		if (const auto it = _macros.find(signature); it != _macros.end())
			return it->second;
		return std::nullopt;
	}

	void MacroTable::importMacros(const TranslationUnit& translationUnit)
	{
		const auto& externMacroTable = translationUnit.macroTable();
		for (const auto& [signature, macro] : externMacroTable._macros)
		{
			if (const auto [it, inserted] = _macros.emplace(signature, macro); !inserted)
				error(0, "Macro redefinition: {}", it->first.name);
		}
	}

	void MacroTable::checkRedefinition(u32 line, const MacroSignature& signature) const
	{
		if (_macros.contains(signature))
			error(line, "Macro redefinition: {}", signature.name);
	}
}

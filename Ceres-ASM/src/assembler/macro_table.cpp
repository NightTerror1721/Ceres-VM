#include "macro_table.h"
#include "translation_unit.h"

namespace ceres::casm
{
	void MacroTable::defineMacro(MacroSignature&& signature, std::vector<std::string>&& parameters, std::vector<Statement>&& body)
	{
		checkRedefinition(0, signature);

		// Read what we need before `signature` is moved into the map.
		const u32 expectedParameterCount = signature.parameterCount;
		const usize actualParameterCount = parameters.size();

		if (actualParameterCount != expectedParameterCount)
			error(0, "Macro parameter count mismatch for '{}': expected {}, got {}", signature.name, expectedParameterCount, actualParameterCount);

		Macro macro = Macro::make(MacroSignature(signature), std::move(parameters), std::move(body));
		auto [it, inserted] = _macros.emplace(std::move(signature), std::move(macro));
		if (!inserted)
			error(0, "Macro redefinition: {}", it->first.name);
	}

	OptionalConstRef<Macro> MacroTable::getMacro(const MacroSignature& signature) const noexcept
	{
		if (const auto it = _macros.find(signature); it != _macros.end())
			return it->second;
		return std::nullopt;
	}

	void MacroTable::checkRedefinition(u32 line, const MacroSignature& signature) const
	{
		if (_macros.contains(signature))
			error(line, "Macro redefinition: {}", signature.name);
	}
}

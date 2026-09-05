#include "assembly_state.h"
#include "translation_unit.h"

namespace ceres::casm
{
	OptionalRef<std::string> AssemblyState::getSourceFile(const std::string& filePath) noexcept
	{
		if (auto it = _sourceFileCache.find(filePath); it != _sourceFileCache.end())
			return OptionalRef<std::string>(it->second);
		return std::nullopt;
	}

	OptionalConstRef<std::string> AssemblyState::getSourceFile(const std::string& filePath) const noexcept
	{
		if (auto it = _sourceFileCache.find(filePath); it != _sourceFileCache.end())
			return OptionalConstRef<std::string>(it->second);
		return std::nullopt;
	}

	OptionalRef<TranslationUnit> AssemblyState::getTranslationUnit(const std::string& filePath) noexcept
	{
		if (auto it = _translationUnitCache.find(filePath); it != _translationUnitCache.end())
			return OptionalRef<TranslationUnit>(it->second);
		return std::nullopt;
	}

	OptionalConstRef<TranslationUnit> AssemblyState::getTranslationUnit(const std::string& filePath) const noexcept
	{
		if (auto it = _translationUnitCache.find(filePath); it != _translationUnitCache.end())
			return OptionalConstRef<TranslationUnit>(it->second);
		return std::nullopt;
	}

	std::string& AssemblyState::cacheSourceFile(const std::string& filePath, std::string&& source) noexcept
	{
		auto [it, inserted] = _sourceFileCache.emplace(filePath, std::move(source));
		return it->second;
	}

	TranslationUnit& AssemblyState::cacheTranslationUnit(const std::string& filePath, TranslationUnit&& translationUnit) noexcept
	{
		auto [it, inserted] = _translationUnitCache.emplace(filePath, std::move(translationUnit));
		return it->second;
	}
}

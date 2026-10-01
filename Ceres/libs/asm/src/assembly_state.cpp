#include <ceres/asm/assembly_state.h>
#include <ceres/asm/translation_unit.h>
#include <system_error>

namespace ceres::casm
{
	// `import "path"` resolves against the importing file first - a module travels with the files it
	// belongs to, not with the working directory - and then against the search directories, which is
	// how a program reaches a library installed somewhere else (`--stdlib`'s lib, `-I`). The
	// assembler's three callers all go through here so the path a diagnostic names is the path that
	// was actually loaded.
	std::string AssemblyState::resolveModulePath(const std::filesystem::path& fromFile, std::string_view moduleName) const
	{
		const std::filesystem::path module{ moduleName };
		if (module.is_absolute())
			return module.lexically_normal().string();

		// A regular file, not merely something that exists: a directory named like the module must not
		// answer an import, or the search directories would never be consulted and the failure would
		// surface later as "Could not open source file".
		std::error_code error;
		const auto relative = fromFile.empty() ? std::filesystem::path{} : fromFile.parent_path() / module;
		if (!relative.empty() && std::filesystem::is_regular_file(relative, error))
			return relative.lexically_normal().string();

		for (const auto& directory : _importDirectories)
		{
			const auto candidate = directory / module;
			if (std::filesystem::is_regular_file(candidate, error))
				return candidate.lexically_normal().string();
		}

		// Nowhere: the file-relative spelling, so "Failed to load module" says where it first looked.
		return relative.empty() ? module.lexically_normal().string() : relative.lexically_normal().string();
	}

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
		if (inserted)
			_translationUnitOrder.push_back(it->first);
		return it->second;
	}
}

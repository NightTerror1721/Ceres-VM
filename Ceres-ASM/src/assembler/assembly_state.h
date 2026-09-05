#pragma once

#include "common/types.h"
#include "strings_pool.h"
#include "symbol_table.h"
#include "translation_unit.h"
#include "errors.h"
#include <unordered_map>
#include <string>
#include <string_view>
#include <functional>
#include <ranges>
#include <vector>

namespace ceres::casm
{
	class TranslationUnit;

	struct MemoryMap
	{
		vm::Address textStart = vm::Address::Null;
		vm::Address rodataStart = vm::Address::Null;
		vm::Address dataStart = vm::Address::Null;
		vm::Address bssStart = vm::Address::Null;

		u32 textSize = 0;
		u32 rodataSize = 0;
		u32 dataSize = 0;
		u32 bssSize = 0;
	};

	class AssemblyState
	{
	public:
		using LoadTranslationUnitFn = std::function<OptionalRef<TranslationUnit>(const std::string&)>;

	private:
		std::unordered_map<std::string, std::string> _sourceFileCache; // Cache for source file contents
		std::unordered_map<std::string, TranslationUnit> _translationUnitCache; // Cache for translation units
		std::vector<std::string> _translationUnitOrder; // Load order, so linking is reproducible
		MemoryMap _memoryMap;
		SymbolTable _globalSymbolTable;
		StringPool _stringPool;
		LoadTranslationUnitFn _loadTranslationUnitFn;
		AssemblerErrorHandler _errorHandler;

	public:
		AssemblyState() = delete;
		AssemblyState(const AssemblyState&) = delete;
		AssemblyState(AssemblyState&&) = default;
		~AssemblyState() = default;

		AssemblyState& operator=(const AssemblyState&) = delete;
		AssemblyState& operator=(AssemblyState&&) = default;

	public:
		OptionalRef<std::string> getSourceFile(const std::string& filePath) noexcept;
		OptionalConstRef<std::string> getSourceFile(const std::string& filePath) const noexcept;

		OptionalRef<TranslationUnit> getTranslationUnit(const std::string& filePath) noexcept;
		OptionalConstRef<TranslationUnit> getTranslationUnit(const std::string& filePath) const noexcept;

		std::string& cacheSourceFile(const std::string& filePath, std::string&& source) noexcept;

		TranslationUnit& cacheTranslationUnit(const std::string& filePath, TranslationUnit&& translationUnit) noexcept;

		// Iterating the cache directly would walk an unordered_map, whose order is unspecified and
		// can differ between runs. Section offsets are assigned during this walk, so the binary has
		// to be built in a fixed order to be reproducible.
		auto translationUnits() const noexcept
		{
			return _translationUnitOrder | std::views::transform(
				[this](const std::string& path) -> const TranslationUnit& { return _translationUnitCache.at(path); });
		}
		auto translationUnits() noexcept
		{
			return _translationUnitOrder | std::views::transform(
				[this](const std::string& path) -> TranslationUnit& { return _translationUnitCache.at(path); });
		}

	public:
		AssemblyState(const LoadTranslationUnitFn& loadTranslationUnitFn) noexcept :
			_loadTranslationUnitFn(loadTranslationUnitFn)
		{}

		MemoryMap& memoryMap() noexcept { return _memoryMap; }
		const MemoryMap& memoryMap() const noexcept { return _memoryMap; }

		SymbolTable& globalSymbolTable() noexcept { return _globalSymbolTable; }
		const SymbolTable& globalSymbolTable() const noexcept { return _globalSymbolTable; }

		StringPool& stringPool() noexcept { return _stringPool; }
		const StringPool& stringPool() const noexcept { return _stringPool; }

		AssemblerErrorHandler& errorHandler() noexcept { return _errorHandler; }
		const AssemblerErrorHandler& errorHandler() const noexcept { return _errorHandler; }

		bool hasSourceFile(const std::string& filePath) const noexcept
		{
			return _sourceFileCache.contains(filePath);
		}

		bool hasTranslationUnit(const std::string& filePath) const noexcept
		{
			return _translationUnitCache.contains(filePath);
		}

		OptionalRef<TranslationUnit> loadTranslationUnit(const std::string& filePath) noexcept
		{
			if (_loadTranslationUnitFn)
				return _loadTranslationUnitFn(filePath);
			return std::nullopt;
		}

	public:
		forceinline Identifier makeIdentifier(std::string_view name) noexcept { return _stringPool.makeIdentifier(name); }
		forceinline Identifier makeIdentifier(const std::string& name) noexcept { return _stringPool.makeIdentifier(name); }
		forceinline Identifier makeIdentifier(std::string&& name) noexcept { return _stringPool.makeIdentifier(std::move(name)); }

		forceinline LiteralString makeLiteralString(std::string_view str) noexcept { return _stringPool.makeLiteralString(str); }
		forceinline LiteralString makeLiteralString(const std::string& str) noexcept { return _stringPool.makeLiteralString(str); }
		forceinline LiteralString makeLiteralString(std::string&& str) noexcept { return _stringPool.makeLiteralString(std::move(str)); }
	};
}

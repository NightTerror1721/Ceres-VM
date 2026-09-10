#pragma once

#include <ceres/core/base/types.h>
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
#include <algorithm>

namespace ceres::casm
{
	using namespace isa;

	class TranslationUnit;

	struct MemoryMap
	{
		Address textStart = Address::Null;
		Address rodataStart = Address::Null;
		Address dataStart = Address::Null;
		Address bssStart = Address::Null;

		u32 textSize = 0;
		u32 rodataSize = 0;
		u32 dataSize = 0;
		u32 bssSize = 0;
	};

	// One resolved `interrupt` binding. Built fresh by Linker::resolveInterruptVectors() every time
	// it runs, so it only ever reflects the final, fully-relocated layout - never a
	// pre-relaxation one.
	//
	// In a whole-program build, `address` is already final and BinaryEmitter uses it directly to
	// build an InterruptVectorPatch for the .cres file. Assembled as an object, nothing here is
	// final yet - `address` is only this object's own placeholder layout (relative to 0), and
	// `external`/`section`/`symbol` are exactly what BinaryEmitter needs to record an
	// ObjectInterruptBinding instead, the same way an ordinary address-bearing field becomes a
	// Relocation rather than a number.
	struct InterruptVectorBinding
	{
		u8 number;
		Address address;
		SectionType section = SectionType::Text;
		// True when the handler is defined in a different translation unit than the one that
		// declared the `interrupt` binding - meaningless in a whole-program build (the address is
		// final either way), load-bearing when assembled as an object.
		bool external = false;
		// Kept in both cases: an error about an interrupt binding is nearly useless without saying
		// which handler it was trying to reach.
		std::string symbol;
	};

	class AssemblyState
	{
	public:
		using LoadTranslationUnitFn = std::function<OptionalRef<TranslationUnit>(const std::string&)>;

	private:
		std::unordered_map<std::string, std::string> _sourceFileCache; // Cache for source file contents
		std::unordered_map<std::string, TranslationUnit> _translationUnitCache; // Cache for translation units
		std::vector<std::string> _translationUnitOrder; // Load order, so linking is reproducible
		std::vector<std::string> _unitsBeingLoaded; // Import cycle guard: a unit is cached only once built
		MemoryMap _memoryMap;
		SymbolTable _globalSymbolTable;
		std::vector<InterruptVectorBinding> _interruptVectors;
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

		// A view of the *stable* copy of filePath already sitting in the source-file cache's key
		// storage (unordered_map never moves or reallocates a key), so every Statement,
		// RelocatableStatement and diagnostic can hold a cheap view instead of its own copy of the
		// path. Only valid to call once cacheSourceFile has cached this exact path (it always has,
		// by the time anything downstream of loadTranslationUnit needs a file view).
		std::string_view internedPath(const std::string& filePath) const noexcept
		{
			auto it = _sourceFileCache.find(filePath);
			return it != _sourceFileCache.end() ? std::string_view(it->first) : std::string_view();
		}

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

		std::vector<InterruptVectorBinding>& interruptVectors() noexcept { return _interruptVectors; }
		const std::vector<InterruptVectorBinding>& interruptVectors() const noexcept { return _interruptVectors; }

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

		// A unit is only added to the cache once it has finished building, so a cycle would
		// otherwise recurse until the stack ran out.
		bool isBeingLoaded(const std::string& filePath) const noexcept
		{
			return std::find(_unitsBeingLoaded.begin(), _unitsBeingLoaded.end(), filePath) != _unitsBeingLoaded.end();
		}

		void beginLoading(const std::string& filePath) { _unitsBeingLoaded.push_back(filePath); }

		void endLoading(const std::string& filePath) noexcept
		{
			if (const auto it = std::find(_unitsBeingLoaded.begin(), _unitsBeingLoaded.end(), filePath); it != _unitsBeingLoaded.end())
				_unitsBeingLoaded.erase(it);
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

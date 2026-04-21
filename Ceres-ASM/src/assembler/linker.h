#pragma once

#include "translation_unit.h"
#include <optional>
#include <format>

namespace ceres::casm
{
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

	class LinkedExecutable
	{
	private:
		std::vector<TranslationUnit> _units;
		SymbolTable _globalSymbolTable;
		MemoryMap _memoryMap;

	public:
		LinkedExecutable() = default;
		LinkedExecutable(const LinkedExecutable&) noexcept = delete;
		LinkedExecutable(LinkedExecutable&&) noexcept = default;
		~LinkedExecutable() noexcept = default;

		LinkedExecutable& operator=(const LinkedExecutable&) noexcept = delete;
		LinkedExecutable& operator=(LinkedExecutable&&) noexcept = default;

	public:
		explicit LinkedExecutable(std::vector<TranslationUnit>&& units, SymbolTable&& globalSymbolTable, MemoryMap&& memoryMap) noexcept :
			_units(std::move(units)),
			_globalSymbolTable(std::move(globalSymbolTable)),
			_memoryMap(std::move(memoryMap))
		{}

		inline std::span<const TranslationUnit> units() const noexcept { return _units; }
		inline const SymbolTable& globalSymbolTable() const noexcept { return _globalSymbolTable; }
		inline const MemoryMap& memoryMap() const noexcept { return _memoryMap; }

		std::vector<TranslationUnit>& units() noexcept { return _units; }
		SymbolTable& globalSymbolTable() noexcept { return _globalSymbolTable; }
		MemoryMap& memoryMap() noexcept { return _memoryMap; }
	};

	class Linker
	{
	private:
		struct MemoryOffsets
		{
			Address textOffset;
			Address rodataOffset;
			Address dataOffset;
			Address bssOffset;
		};

	private:
		AssemblerErrorHandler& _errorHandler;

	public:
		Linker() = delete;
		Linker(const Linker&) noexcept = delete;
		Linker(Linker&&) noexcept = default;
		~Linker() noexcept = default;

		Linker& operator=(const Linker&) noexcept = delete;
		Linker& operator=(Linker&&) noexcept = default;

	public:
		explicit Linker(AssemblerErrorHandler& errorHandler) noexcept : _errorHandler(errorHandler) {}

		std::optional<LinkedExecutable> link(std::vector<TranslationUnit>&& units);

	private:
		MemoryMap calculateMemoryMap(const std::vector<TranslationUnit>& units) const;
		MemoryOffsets calculateMemoryOffsets(const MemoryMap& memoryMap) const;

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

		void reportError(u32 line, std::string_view message) noexcept
		{
			_errorHandler.reportError(line, 1, message);
		}

		template <typename... Args>
		void reportError(u32 line, std::string_view formatStr, Args&&... args) noexcept
		{
			std::string message = std::vformat(formatStr, std::make_format_args(args...));
			_errorHandler.reportError(line, 1, message);
		}
	};
}

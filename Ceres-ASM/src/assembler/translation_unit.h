#pragma once

#include "symbol_table.h"
#include "relocatable_statement.h"
#include "size.h"
#include <vector>
#include <string_view>
#include <expected>

namespace ceres::casm
{
	struct SectionSizes
	{	
		u32 textSize = 0; // Size of the text section in bytes
		u32 dataSize = 0; // Size of the data section in bytes
		u32 rodataSize = 0; // Size of the read-only data section in bytes
		u32 bssSize = 0; // Size of the BSS section in bytes
	};

	class TranslationUnit
	{
	private:
		std::string _source; // The source code of the translation unit
		std::vector<RelocatableStatement> _ast; // The abstract syntax tree (AST) of the translation unit
		SymbolTable _symbolTable; // The symbol table for the translation unit
		SectionSizes _sectionSizes; // The sizes of the sections in the translation unit
		std::reference_wrapper<AssemblerErrorHandler> _errorHandler; // The error handler for the translation unit
		std::vector<std::string> _unresolvedSymbols; // List of unresolved symbols in the translation unit

	public:
		TranslationUnit() = delete;
		TranslationUnit(const TranslationUnit&) noexcept = delete;
		TranslationUnit(TranslationUnit&&) noexcept = default;
		~TranslationUnit() noexcept = default;

		TranslationUnit& operator=(const TranslationUnit&) noexcept = delete;
		TranslationUnit& operator=(TranslationUnit&&) noexcept = default;

	public:
		explicit TranslationUnit(const std::string& source, AssemblerErrorHandler& errorHandler) noexcept : _source(source), _errorHandler(errorHandler) {}
		explicit TranslationUnit(std::string&& source, AssemblerErrorHandler& errorHandler) noexcept : _source(std::move(source)), _errorHandler(errorHandler) {}

		inline std::string_view source() const noexcept { return _source; }
		inline std::span<const RelocatableStatement> ast() const noexcept { return _ast; }
		inline const SymbolTable& symbolTable() const noexcept { return _symbolTable; }
		inline const SectionSizes& sectionSizes() const noexcept { return _sectionSizes; }
		inline const AssemblerErrorHandler& errorHandler() const noexcept { return _errorHandler.get(); }
		inline std::span<const std::string> unresolvedSymbols() const noexcept { return _unresolvedSymbols; }

		inline SymbolTable& symbolTable() noexcept { return _symbolTable; }
		inline SectionSizes& sectionSizes() noexcept { return _sectionSizes; }
		inline AssemblerErrorHandler& errorHandler() noexcept { return _errorHandler.get(); }
		inline std::vector<RelocatableStatement>& ast() noexcept { return _ast; }

		inline void setAST(std::vector<RelocatableStatement>&& ast) noexcept { _ast = std::move(ast); }
		inline void setUnresolvedSymbols(std::vector<std::string>&& symbols) noexcept { _unresolvedSymbols = std::move(symbols); }
	};

	class TranslationUnitBuilder
	{
	private:
		TranslationUnit _translationUnit;
		Address _textOffset = 0; // Current offset in the text section
		Address _dataOffset = 0; // Current offset in the data section
		Address _rodataOffset = 0; // Current offset in the read-only data section
		Address _bssOffset = 0; // Current offset in the BSS section
		std::optional<SectionType> _currentSection; // Current section being processed
		bool _built = false; // Flag indicating whether the translation unit has been built
		bool _released = false; // Flag indicating whether the translation unit has been released

	public:
		TranslationUnitBuilder() = delete;
		TranslationUnitBuilder(const TranslationUnitBuilder&) noexcept = delete;
		TranslationUnitBuilder(TranslationUnitBuilder&&) noexcept = default;
		~TranslationUnitBuilder() noexcept = default;

		TranslationUnitBuilder& operator=(const TranslationUnitBuilder&) noexcept = delete;
		TranslationUnitBuilder& operator=(TranslationUnitBuilder&&) noexcept = default;

	public:
		explicit TranslationUnitBuilder(const std::string& source, AssemblerErrorHandler& errorHandler) noexcept : _translationUnit(source, errorHandler) {}
		explicit TranslationUnitBuilder(std::string&& source, AssemblerErrorHandler& errorHandler) noexcept : _translationUnit(std::move(source), errorHandler) {}

		void build(std::vector<Statement>&& statements);

		TranslationUnit release()
		{
			if (!_built)
				throw std::logic_error("Translation unit has not been built yet");

			if (_released)
				throw std::logic_error("Translation unit has already been released");

			_released = true;

			return std::move(_translationUnit);
		}

	private:
		DataType resolveDataType(u32 line, const DataTypeReference& dataType) const;
		LiteralValue resolveLiteralValue(u32 line, const LiteralValueReference& value, bool allowEmptyArrays = false) const;
		std::pair<DataType, LiteralValue> resolveLiteralValue(u32 line, const DataTypeReference& expectedDataType, const LiteralValueReference& value) const;

		std::expected<u32, std::string_view> sizeOf(u32 line, DataType dataType) const;
		std::expected<u32, std::string_view> sizeOf(u32 line, const LiteralValue& value) const;
		std::expected<u32, std::string_view> sizeOf(u32 line, DataType dataType, const LiteralValue& value) const;

		std::optional<std::reference_wrapper<const LiteralValue>> getConstantValue(u32 line, std::string_view name) const noexcept;

		Address& currentOffset()
		{
			if (!_currentSection.has_value())
				throw std::logic_error("Current section is not set");

			switch (_currentSection.value())
			{
				case SectionType::Text: return _textOffset;
				case SectionType::Data: return _dataOffset;
				case SectionType::Rodata: return _rodataOffset;
				case SectionType::BSS: return _bssOffset;
			}

			std::unreachable();
		}

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

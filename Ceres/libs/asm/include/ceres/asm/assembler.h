#pragma once

#include "parser.h"
#include "translation_unit.h"
#include "linker.h"
#include "binary_emitter.h"
#include "object_file.h"
#include "assembly_state.h"
#include <ceres/core/format/debug_info.h>
#include <filesystem>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace ceres::casm
{
	using namespace fmt;

	struct AssemblerOptions
	{
		std::filesystem::path outputPath;
		// Costs an extra table the size of .text and is of no use to a plain build, so the caller
		// has to ask (ceres asm --debug, or anything that is about to start a debug session).
		bool emitDebugInfo = false;
		// An entry point is a property of a *program*, not of a translation unit: a library module
		// has no `main` and is not wrong for it. Checking a single file (ceres asm with no -o, which
		// is what the language server runs on every open document) would otherwise report a missing
		// entry point on every module that is not the main one.
		bool requireEntryPoint = true;
	};

	class Assembler
	{
	private:
		AssemblerOptions _options;
		std::unique_ptr<AssemblyState> _state = nullptr;
		// Outlives _state deliberately: the tables own their strings, so the debug information
		// stays usable after the assembly state that produced it has been thrown away.
		DebugInfo _debugInfo;

		// Name to register, for the aliases a file publishes with `global alias`.
		using AliasMap = std::unordered_map<std::string, Operand>;

		// Memoised per resolved path, and guarded against a cycle: two files that import each
		// other are a diagnostic the builder reports, not a reason for this to recurse forever.
		std::unordered_map<std::string, AliasMap> _globalAliasCache;
		std::unordered_set<std::string> _aliasScanInProgress;

	public:
		Assembler() = default;
		Assembler(const Assembler&) = delete;
		Assembler(Assembler&&) = default;
		~Assembler() = default;

		Assembler& operator=(const Assembler&) = delete;
		Assembler& operator=(Assembler&&) = default;

	public:
		explicit Assembler(const AssemblerOptions& options) noexcept : _options(options) {}
		explicit Assembler(AssemblerOptions&& options) noexcept : _options(std::move(options)) {}

		std::optional<Program> assemble(std::span<const std::filesystem::path> sourceFiles);

		// One file, assembled on its own into something a later link finishes. What it imports is
		// read for what it declares and contributes no bytes, the way a header does - so the same
		// library can be imported by every object in a program without any of them carrying a
		// copy of it.
		std::optional<ObjectFile> assembleObject(const std::filesystem::path& sourceFile);

	public:
		std::optional<Program> assemble(const std::vector<std::filesystem::path>& sourceFiles)
		{
			return assemble(std::span<const std::filesystem::path>(sourceFiles));
		}
		std::optional<Program> assemble(std::initializer_list<const std::filesystem::path> sourceFiles)
		{
			return assemble(std::span<const std::filesystem::path>(sourceFiles));
		}

		// Empty unless AssemblerOptions::emitDebugInfo was set and assemble() succeeded.
		const DebugInfo& debugInfo() const noexcept { return _debugInfo; }

		bool hasErrors() const noexcept { return _state ? _state->errorHandler().hasErrors() : false; }
		std::span<const AssemblerErrorEntry> errors() const noexcept { return _state ? _state->errorHandler().errors() : std::span<const AssemblerErrorEntry>{}; }

	private:
		std::optional<std::string> readSourceFile(const std::filesystem::path& filePath);
		std::vector<Statement> parseSource(const std::string& source, const std::filesystem::path& filePath);

		// Every `global alias` a file publishes, plus the ones it inherited from its own imports.
		//
		// This is the one declaration that cannot be resolved the way the others are. A register
		// alias is substituted where it is written, and the parser needs it *while parsing*: what
		// `[cursor + 4]` even is - a register base or a symbolic address - depends on knowing that
		// `cursor` is a register. Every other symbol is looked up long after parsing, when there
		// is a symbol table to look in; this one has to arrive first, so an importing file's
		// imports are scanned for aliases before a single line of it is parsed.
		//
		// The scan lexes rather than parses: it needs four tokens in a row, and parsing an
		// imported file here would mean parsing it before the aliases *it* imported were known.
		const AliasMap& collectGlobalAliases(const std::string& resolvedPath);
		AliasMap collectImportedAliases(const std::string& resolvedPath);
		std::optional<TranslationUnit> translateStatementsToUnit(const std::string& source, std::vector<Statement>&& statements, const std::filesystem::path& filePath);
		bool linkTranslationUnits();
		bool linkTranslationUnitsForObject(std::string_view rootFile);
		void warnAboutUnusedPrivateDeclarations();
		std::optional<Program> emitBinary();

		OptionalRef<TranslationUnit> loadTranslationUnit(const std::string& filePath) noexcept;

	private:
		// Assembler-level failures (no source files, could not open/parse/link/emit) aren't tied to
		// one statement's file - the ones that are file-specific already name it in the message
		// text (e.g. "Could not open source file '{}'"), so there's no separate file field here.
		void reportError(std::string_view message) noexcept
		{
			if (_state)
				_state->errorHandler().reportError("", 1, 1, message);
		}

		template <typename... Args>
		void reportError(std::string_view formatStr, Args&&... args) noexcept
		{
			std::string message = std::vformat(formatStr, std::make_format_args(args...));
			if (_state)
				_state->errorHandler().reportError("", 1, 1, message);
		}
	};
}

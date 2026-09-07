#pragma once

#include "parser.h"
#include "translation_unit.h"
#include "linker.h"
#include "binary_emitter.h"
#include "assembly_state.h"
#include "debug/debug_info.h"
#include <filesystem>
#include <vector>
#include <memory>

namespace ceres::casm
{
	struct AssemblerOptions
	{
		std::filesystem::path outputPath;
		// Costs an extra table the size of .text and is of no use to a plain build, so the caller
		// has to ask (ceres asm --debug, or anything that is about to start a debug session).
		bool emitDebugInfo = false;
	};

	class Assembler
	{
	private:
		AssemblerOptions _options;
		std::unique_ptr<AssemblyState> _state = nullptr;
		// Outlives _state deliberately: the tables own their strings, so the debug information
		// stays usable after the assembly state that produced it has been thrown away.
		debug::DebugInfo _debugInfo;

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

		std::optional<vm::Program> assemble(std::span<const std::filesystem::path> sourceFiles);

	public:
		std::optional<vm::Program> assemble(const std::vector<std::filesystem::path>& sourceFiles)
		{
			return assemble(std::span<const std::filesystem::path>(sourceFiles));
		}
		std::optional<vm::Program> assemble(std::initializer_list<const std::filesystem::path> sourceFiles)
		{
			return assemble(std::span<const std::filesystem::path>(sourceFiles));
		}

		// Empty unless AssemblerOptions::emitDebugInfo was set and assemble() succeeded.
		const debug::DebugInfo& debugInfo() const noexcept { return _debugInfo; }

		bool hasErrors() const noexcept { return _state ? _state->errorHandler().hasErrors() : false; }
		std::span<const AssemblerErrorEntry> errors() const noexcept { return _state ? _state->errorHandler().errors() : std::span<const AssemblerErrorEntry>{}; }

	private:
		std::optional<std::string> readSourceFile(const std::filesystem::path& filePath);
		std::vector<Statement> parseSource(const std::string& source, const std::filesystem::path& filePath);
		std::optional<TranslationUnit> translateStatementsToUnit(const std::string& source, std::vector<Statement>&& statements, const std::filesystem::path& filePath);
		bool linkTranslationUnits();
		std::optional<vm::Program> emitBinary();

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

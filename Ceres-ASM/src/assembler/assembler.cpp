#include "assembler.h"
#include <format>
#include <fstream>

namespace ceres::casm
{
	std::optional<vm::Program> Assembler::assemble(std::span<const std::filesystem::path> sourceFiles)
	{
		_state.reset();
		_debugInfo = debug::DebugInfo{};
		_state = std::make_unique<AssemblyState>(
			[&](const std::string& filePath) -> OptionalRef<TranslationUnit>
			{
				return loadTranslationUnit(filePath);
			}
		);

		if (sourceFiles.empty())
		{
			reportError("No source files provided for assembly");
			return std::nullopt;
		}

		for (const auto& filePath : sourceFiles)
			loadTranslationUnit(filePath.string());

		if (hasErrors())
			return std::nullopt;

		if (!linkTranslationUnits())
		{
			reportError("Linking failed due to unresolved symbols or other errors");
			return std::nullopt;
		}

		warnAboutUnusedPrivateDeclarations();

		auto binaryOpt = emitBinary();
		if (!binaryOpt.has_value() || hasErrors())
			return std::nullopt;

		return binaryOpt;
	}

	// `global` created a category the language did not have before: a declaration that provably
	// cannot be reached from anywhere else. A private one nobody names in its own file is dead with
	// certainty, which is not something that could be said of anything until now.
	void Assembler::warnAboutUnusedPrivateDeclarations()
	{
		if (!_state)
			return;

		auto& errorHandler = _state->errorHandler();

		for (const auto& unit : _state->translationUnits())
		{
			for (const auto& [name, symbol] : unit.symbolTable().getAllSymbols())
			{
				if (symbol.isGlobal() || symbol.uses() > 0)
					continue;
				if (!symbol.isConstant() && !symbol.isVariable())
					continue; // A label that is never jumped to is often an entry point or a marker.
				// A dotted name is generated, not written: a struct's field offsets, or a local
				// label. Warning about each unused field of a record would drown the useful ones.
				if (name.find('.') != std::string::npos)
					continue;

				errorHandler.reportWarning(unit.file(), symbol.line(), 1,
					std::format("'{}' is declared but never used, and is not global, so nothing outside this file can use it either", name));
			}

			for (const auto& [signature, macro] : unit.macroTable().getAllMacros())
			{
				if (macro.isGlobal() || macro.uses() > 0)
					continue;

				errorHandler.reportWarning(unit.file(), 0, 1,
					std::format("macro '{}' is declared but never used, and is not global", signature.name));
			}
		}
	}

	std::optional<std::string> Assembler::readSourceFile(const std::filesystem::path& filePath)
	{
		try
		{
			std::ifstream file(filePath);
			if (!file.is_open())
			{
				reportError("Could not open source file '{}'", filePath.string());
				return std::nullopt;
			}

			std::string source{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
			return source;
		}
		catch (const std::exception& e)
		{
			reportError("Error reading source file '{}': {}", filePath.string(), e.what());
			return std::nullopt;
		}
	}

	std::vector<Statement> Assembler::parseSource(const std::string& source, const std::filesystem::path& filePath)
	{
		try
		{
			// A view into the source-file cache's stable key, not filePath.string() directly - that
			// would be a temporary, and every Statement the parser builds holds this view onward
			// without copying it.
			Parser parser{ source, _state->stringPool(), _state->errorHandler(), _state->internedPath(filePath.string()) };
			return parser.parse();
		}
		catch (const std::exception& e)
		{
			reportError("Error parsing source file '{}': {}", filePath.string(), e.what());
			return {};
		}
	}

	std::optional<TranslationUnit> Assembler::translateStatementsToUnit(const std::string& source, std::vector<Statement>&& statements, const std::filesystem::path& filePath)
	{
		try
		{
			TranslationUnitBuilder builder{ *_state, filePath, _state->internedPath(filePath.string()) };
			builder.build(std::move(statements));
			return builder.release();
		}
		catch (const std::exception& e)
		{
			reportError("Error translating statements to translation unit for file '{}': {}", filePath.string(), e.what());
			return std::nullopt;
		}
	}

	bool Assembler::linkTranslationUnits()
	{
		try
		{
			Linker linker{ *_state };
			return linker.link();
		}
		catch (const std::exception& e)
		{
			reportError("Error linking translation units: {}", e.what());
			return false;
		}
	}

	std::optional<vm::Program> Assembler::emitBinary()
	{
		try
		{
			BinaryEmitter emitter{ *_state, _options.emitDebugInfo, _options.requireEntryPoint };
			auto program = emitter.emit();
			if (program.has_value())
				_debugInfo = emitter.takeDebugInfo();
			return program;
		}
		catch (const std::exception& e)
		{
			reportError("Error emitting binary: {}", e.what());
			return std::nullopt;
		}
	}

	OptionalRef<TranslationUnit> Assembler::loadTranslationUnit(const std::string& filePath) noexcept
	{
		if (!_state)
			return std::nullopt;

		auto result = _state->getTranslationUnit(filePath);
		if (result.has_value())
			return result;

		auto sourceOpt = readSourceFile(filePath);
		if (!sourceOpt.has_value())
			return std::nullopt;

		auto& source = _state->cacheSourceFile(filePath, std::move(sourceOpt.value()));

		auto statements = parseSource(source, filePath);
		if (hasErrors())
			return std::nullopt;

		// Marked in progress across the build so a circular import is reported rather than
		// recursing until the stack runs out.
		_state->beginLoading(filePath);
		auto translationUnit = translateStatementsToUnit(source, std::move(statements), filePath);
		_state->endLoading(filePath);

		if (!translationUnit.has_value())
			return std::nullopt;

		return _state->cacheTranslationUnit(filePath, std::move(translationUnit.value()));
	}
}

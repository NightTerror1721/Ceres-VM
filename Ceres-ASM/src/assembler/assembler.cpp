#include "assembler.h"
#include <fstream>

namespace ceres::casm
{
	std::optional<vm::Program> Assembler::assemble(std::span<const std::filesystem::path> sourceFiles)
	{
		_state.reset();
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

		auto binaryOpt = emitBinary();
		if (!binaryOpt.has_value() || hasErrors())
			return std::nullopt;

		return binaryOpt;
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
			Parser parser{ source, _state->stringPool(), _state->errorHandler() };
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
			TranslationUnitBuilder builder{ *_state };
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
			BinaryEmitter emitter{ *_state };
			return emitter.emit();
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

		auto translationUnit = translateStatementsToUnit(source, std::move(statements), filePath);
		if (!translationUnit.has_value())
			return std::nullopt;

		return _state->cacheTranslationUnit(filePath, std::move(translationUnit.value()));
	}
}

#include "assembler.h"
#include <fstream>

namespace ceres::casm
{
	std::optional<vm::Program> Assembler::assemble(std::span<const std::filesystem::path> sourceFiles)
	{
		_errorHandler.clearErrors();

		if (sourceFiles.empty())
		{
			reportError("No source files provided for assembly");
			return std::nullopt;
		}

		std::vector<TranslationUnit> translationUnits;
		for (const auto& filePath : sourceFiles)
		{
			auto sourceOpt = readSourceFile(filePath);
			if (!sourceOpt.has_value())
				continue; // Continue to the next file instead of returning immediately

			auto source = std::move(sourceOpt.value());

			auto statements = parseSource(source, filePath);
			if (_errorHandler.hasErrors())
				continue; // Continue to the next file if there were parsing errors

			auto translationUnit = translateStatementsToUnit(source, std::move(statements), filePath);
			translationUnits.push_back(std::move(translationUnit));
		}

		if (_errorHandler.hasErrors())
		{
			reportError("Assembly failed due to errors in source files");
			return std::nullopt;
		}

		auto linkedExecutableOpt = linkTranslationUnits(std::move(translationUnits));
		if (!linkedExecutableOpt.has_value())
		{
			reportError("Linking failed due to unresolved symbols or other errors");
			return std::nullopt;
		}

		if (_errorHandler.hasErrors())
		{
			reportError("Linking failed due to errors in translation units");
			return std::nullopt;
		}

		auto binaryOpt = emitBinary(std::move(linkedExecutableOpt.value()));
		if (!binaryOpt.has_value())
		{
			reportError("Binary emission failed due to errors in the linked executable");
			return std::nullopt;
		}

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
			Parser parser{ source, _errorHandler };
			return parser.parse();
		}
		catch (const std::exception& e)
		{
			reportError("Error parsing source file '{}': {}", filePath.string(), e.what());
			return {};
		}
	}

	TranslationUnit Assembler::translateStatementsToUnit(const std::string& source, std::vector<Statement>&& statements, const std::filesystem::path& filePath)
	{
		try
		{
			TranslationUnitBuilder builder{ source, _errorHandler };
			builder.build(std::move(statements));
			return builder.release();
		}
		catch (const std::exception& e)
		{
			reportError("Error translating statements to translation unit for file '{}': {}", filePath.string(), e.what());
			return TranslationUnit{ "", _errorHandler }; // Return an empty translation unit on error
		}
	}

	std::optional<LinkedExecutable> Assembler::linkTranslationUnits(std::vector<TranslationUnit>&& units)
	{
		try
		{
			Linker linker{ _errorHandler };
			return linker.link(std::move(units));
		}
		catch (const std::exception& e)
		{
			reportError("Error linking translation units: {}", e.what());
			return std::nullopt;
		}
	}

	std::optional<vm::Program> Assembler::emitBinary(LinkedExecutable&& linkedExecutable)
	{
		try
		{
			BinaryEmitter emitter{ std::move(linkedExecutable), _errorHandler };
			return emitter.emit();
		}
		catch (const std::exception& e)
		{
			reportError("Error emitting binary: {}", e.what());
			return std::nullopt;
		}
	}
}

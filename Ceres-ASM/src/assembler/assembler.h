#include "parser.h"
#include "translation_unit.h"
#include "linker.h"
#include "binary_emitter.h"
#include <filesystem>

namespace ceres::casm
{
	struct AssemblerOptions
	{
		std::filesystem::path outputPath;
	};

	class Assembler
	{
	private:
		AssemblerOptions _options;
		AssemblerErrorHandler _errorHandler;

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

		bool hasErrors() const noexcept { return _errorHandler.hasErrors(); }
		std::span<const AssemblerErrorEntry> errors() const noexcept { return _errorHandler; }

	private:
		std::optional<std::string> readSourceFile(const std::filesystem::path& filePath);
		std::vector<Statement> parseSource(const std::string& source, const std::filesystem::path& filePath);
		TranslationUnit translateStatementsToUnit(const std::string& source, std::vector<Statement>&& statements, const std::filesystem::path& filePath);
		std::optional<LinkedExecutable> linkTranslationUnits(std::vector<TranslationUnit>&& units);
		std::optional<vm::Program> emitBinary(LinkedExecutable&& linkedExecutable);

	private:
		void reportError(std::string_view message) noexcept
		{
			_errorHandler.reportError(1, 1, message);
		}

		template <typename... Args>
		void reportError(std::string_view formatStr, Args&&... args) noexcept
		{
			std::string message = std::vformat(formatStr, std::make_format_args(args...));
			_errorHandler.reportError(1, 1, message);
		}
	};
}

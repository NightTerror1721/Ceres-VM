#pragma once

// Assembling from a string. The Assembler only takes file paths, so the source is written to a
// temporary file first. Kept here rather than in the assembler itself: a string-based entry point
// is a public API decision, and these tests do not need to make it.

#include "assembler/assembler.h"
#include "debug/debug_info.h"
#include "vm/disassembler.h"
#include "vm/memory.h"
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace ceres::testing
{
	struct AssembleResult
	{
		std::optional<vm::Program> program;
		std::vector<std::string> errors;
		// Kept apart from errors: a warning does not stop a build, so ok() must not care about one.
		std::vector<std::string> warnings;
		// Empty unless assembleSource was asked for debug information. The path is kept because
		// the line table is keyed on it and the temporary file is gone by the time a test looks.
		debug::DebugInfo debugInfo;
		std::string sourcePath;

		bool ok() const noexcept { return program.has_value() && errors.empty(); }

		// The text section as machine words, in program order.
		std::vector<u32> words() const
		{
			std::vector<u32> out;
			if (!program.has_value())
				return out;

			const auto text = program->text();
			for (usize i = 0; i + vm::Instruction::Size <= text.size(); i += vm::Instruction::Size)
			{
				out.push_back(
					static_cast<u32>(text[i]) |
					(static_cast<u32>(text[i + 1]) << 8) |
					(static_cast<u32>(text[i + 2]) << 16) |
					(static_cast<u32>(text[i + 3]) << 24));
			}
			return out;
		}

		std::string listing() const
		{
			if (!program.has_value())
				return "<no program>";
			return vm::Disassembler::listing(program->text(), vm::Memory::UnrestrictedSegmentStart);
		}

		std::string joinedErrors() const { return join(errors, "<no errors>"); }
		std::string joinedWarnings() const { return join(warnings, "<no warnings>"); }

	private:
		static std::string join(const std::vector<std::string>& lines, const char* whenEmpty)
		{
			std::string out;
			for (const auto& line : lines)
			{
				if (!out.empty())
					out += " | ";
				out += line;
			}
			return out.empty() ? whenEmpty : out;
		}
	};

	// What `ceres asm` does with no -o, and what the language server runs on every open document:
	// the file is checked but no runnable program is demanded of it, so a library module with no
	// `main` is not an error.
	inline AssembleResult checkSource(std::string_view source, std::string_view fileStem = "checked");

	inline AssembleResult assembleSource(std::string_view source, std::string_view fileStem = "snippet", bool withDebugInfo = false)
	{
		AssembleResult result;

		std::filesystem::path path =
			std::filesystem::temp_directory_path() / std::format("ceres_test_{}.casm", fileStem);
		result.sourcePath = path.string();

		{
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			if (!file)
			{
				result.errors.push_back("could not open temporary file " + path.string());
				return result;
			}
			file.write(source.data(), static_cast<std::streamsize>(source.size()));
		}

		casm::Assembler assembler{ casm::AssemblerOptions{ .emitDebugInfo = withDebugInfo } };
		result.program = assembler.assemble({ path });
		result.debugInfo = assembler.debugInfo();
		for (const auto& diagnostic : assembler.errors())
		{
			auto& into = diagnostic.isWarning() ? result.warnings : result.errors;
			into.push_back(std::format("[line {}] {}", diagnostic.line, diagnostic.message));
		}

		std::error_code ignored;
		std::filesystem::remove(path, ignored);

		return result;
	}

	// Wraps a body in the minimal scaffolding an assembly unit needs: a @text section and the
	// global entry point the emitter insists on.
	inline std::string inText(std::string_view body)
	{
		return std::format("@text\r\nglobal main:\r\n{}\r\n", body);
	}

	inline std::string renderWord(u32 word)
	{
		return std::format("{:08x}  {}", word, vm::Disassembler::disassemble(vm::Instruction(word)));
	}

	inline AssembleResult checkSource(std::string_view source, std::string_view fileStem)
	{
		AssembleResult result;

		std::filesystem::path path =
			std::filesystem::temp_directory_path() / std::format("ceres_test_{}.casm", fileStem);
		result.sourcePath = path.string();

		{
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			if (!file)
			{
				result.errors.push_back("could not open temporary file " + path.string());
				return result;
			}
			file.write(source.data(), static_cast<std::streamsize>(source.size()));
		}

		casm::Assembler assembler{ casm::AssemblerOptions{ .requireEntryPoint = false } };
		result.program = assembler.assemble({ path });
		for (const auto& diagnostic : assembler.errors())
		{
			auto& into = diagnostic.isWarning() ? result.warnings : result.errors;
			into.push_back(std::format("[line {}] {}", diagnostic.line, diagnostic.message));
		}

		std::error_code ignored;
		std::filesystem::remove(path, ignored);

		return result;
	}
}

#pragma once

#include <ceres/vm/memory.h>
#include <expected>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace ceres::driver
{
	struct AssembleCommand
	{
		std::vector<std::filesystem::path> inputs;
		std::filesystem::path output;
		bool compileOnly = false;
		bool listing = false;
		bool jsonDiagnostics = false;
		bool debugInfo = false;
		bool debugJson = false;
	};

	struct LinkCommand
	{
		std::vector<std::filesystem::path> inputs;
		std::filesystem::path output;
		bool debugInfo = false;
		bool debugJson = false;
	};

	struct ArchiveCommand
	{
		std::filesystem::path output;
		std::vector<std::filesystem::path> inputs;
	};

	struct RunCommand
	{
		std::filesystem::path input;
		usize memorySize = vm::Memory::DefaultSize;
		std::filesystem::path diskImage;
		bool listing = false;
		bool debugInfo = false;
	};

	struct ProfileCommand
	{
		std::filesystem::path input;
		usize memorySize = vm::Memory::DefaultSize;
		bool listing = false;
	};

	struct DisassembleCommand
	{
		std::filesystem::path input;
		bool debugInfo = false;
		bool debugJson = false;
	};

	struct DebugCommand
	{
		std::vector<std::filesystem::path> inputs;
		usize memorySize = vm::Memory::DefaultSize;
		bool stopOnEntry = true;
		bool server = false;
		bool recordHistory = true;
	};

	using Command = std::variant<AssembleCommand, LinkCommand, ArchiveCommand, RunCommand,
		ProfileCommand, DisassembleCommand, DebugCommand>;

	struct ParseError { std::string message; };

	std::expected<Command, ParseError> parseCommandLine(int argc, char* const argv[]);
	std::string_view usageText() noexcept;
}

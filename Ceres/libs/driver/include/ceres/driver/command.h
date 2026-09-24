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
		bool symbolTable = false;   // --symtab
	};

	struct ArchiveCommand
	{
		std::filesystem::path output;
		std::vector<std::filesystem::path> inputs;
	};

	// `--port 0=stick.img` and `--cart 1=game.cart`: a medium plugged into one of the peripheral ports before the program starts.
	struct PortAttachment
	{
		unsigned port = 0;
		std::filesystem::path path;
		bool cartridge = false;   // read only, and has to exist
	};

	struct RunCommand
	{
		std::filesystem::path input;
		usize memorySize = vm::Memory::DefaultSize;
		std::filesystem::path diskImage;
		bool listing = false;
		bool debugInfo = false;
		bool window = false;     // --window: open the window at once
		bool terminal = false;   // --terminal: no window; the text framebuffer goes to the terminal
		std::vector<PortAttachment> ports;
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

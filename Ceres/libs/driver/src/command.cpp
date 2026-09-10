#include <ceres/driver/command.h>

#include <charconv>
#include <string_view>

namespace ceres::driver
{
	namespace
	{
		constexpr std::string_view Usage =
			"Ceres - assembler and virtual machine\n"
			"\n"
			"  ceres asm <source.casm> [<source2.casm> ...] [-o <output.cres>] [--listing] [--json]\n"
			"                          [--debug] [--emit-debug-json] [-c]\n"
			"  ceres link <file.cobj|file.car> [...] -o <output.cres> [--debug]\n"
			"  ceres ar <output.car> <file.cobj> [...]\n"
			"  ceres run <file.casm|file.cres> [--memory <bytes>] [--disk <image>]\n"
			"  ceres profile <file.casm|file.cres> [--memory <bytes>]\n"
			"  ceres disasm <file.casm|file.cres> [--debug]\n"
			"  ceres debug <file.casm|file.cres> [<source2.casm> ...] [--memory <bytes>]\n"
			"                                    [--no-stop-on-entry] [--server] [--no-history]\n"
			"\n"
			"A bare path is shorthand for 'run'.\n";

		struct RawOptions
		{
			std::filesystem::path output;
			std::vector<std::filesystem::path> positional;
			bool compileOnly = false;
			bool usedOutput = false;
			bool listing = false;
			bool usedListing = false;
			bool json = false;
			bool usedJson = false;
			bool debugInfo = false;
			bool usedDebugInfo = false;
			bool debugJson = false;
			bool usedDebugJson = false;
			bool stopOnEntry = true;
			bool usedStopOnEntry = false;
			bool server = false;
			bool usedServer = false;
			bool recordHistory = true;
			bool usedHistory = false;
			usize memorySize = vm::Memory::DefaultSize;
			bool usedMemory = false;
			std::filesystem::path disk;
			bool usedDisk = false;
		};

		std::expected<usize, ParseError> parseMemorySize(std::string_view text)
		{
			usize size = 0;
			const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), size);
			if (error != std::errc{} || end != text.data() + text.size() || size == 0)
				return std::unexpected(ParseError{ "--memory needs a positive integer number of bytes" });
			return size;
		}

		bool isCommand(std::string_view text)
		{
			return text == "asm" || text == "run" || text == "disasm" || text == "debug" ||
				text == "profile" || text == "link" || text == "ar";
		}

		ParseError invalidOption(std::string_view option, std::string_view command)
		{
			return ParseError{ "Option '" + std::string(option) + "' is not valid for '" +
				std::string(command) + "'" };
		}
	}

	std::string_view usageText() noexcept { return Usage; }

	std::expected<Command, ParseError> parseCommandLine(int argc, char* const argv[])
	{
		RawOptions raw;
		for (int i = 1; i < argc; ++i)
		{
			const std::string_view argument = argv[i];
			auto nextValue = [&](std::string_view option) -> std::expected<std::string_view, ParseError>
			{
				if (++i >= argc)
					return std::unexpected(ParseError{ "Missing value after " + std::string(option) });
				return argv[i];
			};

			if (argument == "-o" || argument == "--output")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				raw.output = *value; raw.usedOutput = true;
			}
			else if (argument == "-c") raw.compileOnly = true;
			else if (argument == "--listing") { raw.listing = true; raw.usedListing = true; }
			else if (argument == "--json") { raw.json = true; raw.usedJson = true; }
			else if (argument == "--debug") { raw.debugInfo = true; raw.usedDebugInfo = true; }
			else if (argument == "--emit-debug-json") { raw.debugJson = true; raw.debugInfo = true; raw.usedDebugJson = true; }
			else if (argument == "--no-stop-on-entry") { raw.stopOnEntry = false; raw.usedStopOnEntry = true; }
			else if (argument == "--server") { raw.server = true; raw.usedServer = true; }
			else if (argument == "--no-history") { raw.recordHistory = false; raw.usedHistory = true; }
			else if (argument == "--disk")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				raw.disk = *value; raw.usedDisk = true;
			}
			else if (argument == "--memory")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				auto memory = parseMemorySize(*value);
				if (!memory) return std::unexpected(memory.error());
				raw.memorySize = *memory; raw.usedMemory = true;
			}
			else if (argument == "-h" || argument == "--help")
				return std::unexpected(ParseError{});
			else if (argument.starts_with('-'))
				return std::unexpected(ParseError{ "Unknown option '" + std::string(argument) + "'" });
			else
				raw.positional.emplace_back(argument);
		}

		if (raw.positional.empty())
			return std::unexpected(ParseError{});

		std::string command = "run";
		usize firstInput = 0;
		if (isCommand(raw.positional.front().string()))
		{
			command = raw.positional.front().string();
			firstInput = 1;
		}
		if (raw.positional.size() <= firstInput)
			return std::unexpected(ParseError{ "Missing input file for '" + std::string(command) + "'" });

		std::vector<std::filesystem::path> inputs(raw.positional.begin() + static_cast<std::ptrdiff_t>(firstInput), raw.positional.end());
		if (command == "asm")
		{
			if (raw.usedMemory || raw.usedDisk || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory)
				return std::unexpected(invalidOption("a supplied option", command));
			return AssembleCommand{ std::move(inputs), std::move(raw.output), raw.compileOnly, raw.listing,
				raw.json, raw.debugInfo, raw.debugJson };
		}
		if (command == "link")
		{
			if (raw.compileOnly || raw.usedListing || raw.usedJson || raw.usedMemory || raw.usedDisk || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory)
				return std::unexpected(invalidOption("a supplied option", command));
			if (raw.output.empty()) return std::unexpected(ParseError{ "'ceres link' needs -o <output.cres>" });
			return LinkCommand{ std::move(inputs), std::move(raw.output), raw.debugInfo, raw.debugJson };
		}
		if (command == "ar")
		{
			if (raw.compileOnly || raw.usedListing || raw.usedJson || raw.usedDebugInfo || raw.usedDebugJson || raw.usedDisk ||
				raw.usedMemory || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory || raw.usedOutput)
				return std::unexpected(invalidOption("a supplied option", command));
			if (inputs.size() < 2) return std::unexpected(ParseError{ "'ceres ar' needs an output and at least one object" });
			ArchiveCommand archive{ std::move(inputs.front()), {} };
			archive.inputs.assign(std::make_move_iterator(inputs.begin() + 1), std::make_move_iterator(inputs.end()));
			return archive;
		}
		if (inputs.size() != 1 && command != "debug")
			return std::unexpected(ParseError{ "'" + std::string(command) + "' takes a single input file" });
		if (command == "run")
		{
			if (raw.compileOnly || raw.usedOutput || raw.usedJson || raw.usedDebugJson || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory)
				return std::unexpected(invalidOption("a supplied option", command));
			return RunCommand{ std::move(inputs.front()), raw.memorySize, std::move(raw.disk), raw.listing, raw.debugInfo };
		}
		if (command == "profile")
		{
			if (raw.compileOnly || raw.usedOutput || raw.usedJson || raw.usedDebugInfo || raw.usedDebugJson || raw.usedDisk || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory)
				return std::unexpected(invalidOption("a supplied option", command));
			return ProfileCommand{ std::move(inputs.front()), raw.memorySize, raw.listing };
		}
		if (command == "disasm")
		{
			if (raw.compileOnly || raw.usedOutput || raw.usedListing || raw.usedJson || raw.usedMemory || raw.usedDisk || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory)
				return std::unexpected(invalidOption("a supplied option", command));
			return DisassembleCommand{ std::move(inputs.front()), raw.debugInfo, raw.debugJson };
		}
		if (raw.compileOnly || raw.usedOutput || raw.usedListing || raw.usedJson || raw.usedDebugInfo || raw.usedDebugJson || raw.usedDisk)
			return std::unexpected(invalidOption("a supplied option", command));
		return DebugCommand{ std::move(inputs), raw.memorySize, raw.stopOnEntry, raw.server, raw.recordHistory };
	}
}

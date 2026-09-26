#include <ceres/driver/command.h>

#include <charconv>
#include <chrono>
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
			"  ceres link <file.cobj|file.car> [...] -o <output.cres> [--debug] [--symtab] [--gc-sections]\n"
			"  ceres ar <output.car> <file.cobj> [...]\n"
			"  ceres run <file.casm|file.cres> [--memory <bytes>] [--disk <image>] [--window | --terminal] [--strict-mmio]\n"
			"                                  [--rtc <YYYY-MM-DDThh:mm:ss>]\n"
			"                                  [--port <n>=<image>]... [--cart <n>=<file>]...\n"
			"                                  [--env <name>=<value>]... [--host-dir <dir>] [-- <argument>...]\n"
			"  ceres profile <file.casm|file.cres> [--memory <bytes>]\n"
			"  ceres disasm <file.casm|file.cres> [--debug]\n"
			"  ceres debug <file.casm|file.cres> [<source2.casm> ...] [--memory <bytes>]\n"
			"                                    [--no-stop-on-entry] [--server] [--no-history]\n"
			"\n"
			"A bare path is shorthand for 'run'.\n"
			"\n"
			"With a window (an SDL build), 'run' opens it when the program first shows a frame - of the text\n"
			"framebuffer or of the pixel display - so a program that never does opens none. --window opens it at\n"
			"once; --terminal (or CERES_HEADLESS in the environment) never does, and text frames go to the terminal.\n"
			"\n"
			"Everything after -- goes to the program: main(argc, argv) gets the input's path as argv[0], then those.\n"
			"--env gives it an environment variable (getenv); nothing of the host's environment is passed on.\n"
			"--host-dir lets it open, write and list the host's files under <dir>, and nowhere else.\n"
			"--rtc starts the machine's real-time clock at that moment (UTC) instead of the host's.\n";

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
			bool symbolTable = false;
			bool usedSymbolTable = false;
			bool gcSections = false;
			bool usedGcSections = false;
			bool stopOnEntry = true;
			bool usedStopOnEntry = false;
			bool server = false;
			bool usedServer = false;
			bool recordHistory = true;
			bool usedHistory = false;
			usize memorySize = vm::Memory::DefaultSize;
			bool usedMemory = false;
			std::filesystem::path disk;
			bool usedDisk = false;            // also set by --port and --cart: none of them belongs to any command but run
			std::vector<PortAttachment> ports;
			std::vector<std::string> arguments;
			std::vector<std::string> environment;
			std::filesystem::path hostDirectory;
			bool usedDashDash = false;        // --, --env and --host-dir belong to run alone
			bool usedEnv = false;
			bool usedHostDir = false;
			bool strictMmio = false;
			std::optional<i64> rtc;
			bool window = false;
			bool usedWindow = false;
			bool terminal = false;
			bool usedTerminal = false;
		};

		// YYYY-MM-DDThh:mm:ss, UTC, from 1970 on: the machine's real-time clock counts seconds since then.
		std::expected<i64, ParseError> parseRtc(std::string_view text)
		{
			const auto bad = [&] { return std::unexpected(ParseError{ "'--rtc' takes YYYY-MM-DDThh:mm:ss, for example 2026-09-26T12:00:00, not '" + std::string(text) + "'" }); };
			if (text.size() != 19 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':')
				return bad();
			const auto number = [&](usize at, usize length, int& out)
			{
				const auto [end, error] = std::from_chars(text.data() + at, text.data() + at + length, out);
				return error == std::errc{} && end == text.data() + at + length;
			};
			int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
			if (!number(0, 4, year) || !number(5, 2, month) || !number(8, 2, day) || !number(11, 2, hour) ||
				!number(14, 2, minute) || !number(17, 2, second))
				return bad();
			const std::chrono::year_month_day date{ std::chrono::year{ year }, std::chrono::month{ static_cast<unsigned>(month) },
				std::chrono::day{ static_cast<unsigned>(day) } };
			if (!date.ok() || year < 1970 || hour > 23 || minute > 59 || second > 59)
				return bad();
			const auto days = std::chrono::sys_days{ date }.time_since_epoch();
			return std::chrono::duration_cast<std::chrono::seconds>(days).count() + hour * 3600 + minute * 60 + second;
		}

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

			if (argument == "--")
			{
				// The rest is the program's, options or not.
				for (++i; i < argc; ++i)
					raw.arguments.emplace_back(argv[i]);
				raw.usedDashDash = true;
				break;
			}
			if (argument == "--env")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				const std::string_view text = *value;
				if (text.find('=') == std::string_view::npos || text.front() == '=')
					return std::unexpected(ParseError{ "'--env' takes <name>=<value>, for example --env HOME=/save" });
				raw.environment.emplace_back(text);
				raw.usedEnv = true;
			}
			else if (argument == "--host-dir")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				raw.hostDirectory = *value;
				raw.usedHostDir = true;
			}
			else if (argument == "-o" || argument == "--output")
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
			else if (argument == "--symtab") { raw.symbolTable = true; raw.usedSymbolTable = true; }
			else if (argument == "--gc-sections") { raw.gcSections = true; raw.usedGcSections = true; }
			else if (argument == "--no-stop-on-entry") { raw.stopOnEntry = false; raw.usedStopOnEntry = true; }
			else if (argument == "--server") { raw.server = true; raw.usedServer = true; }
			else if (argument == "--no-history") { raw.recordHistory = false; raw.usedHistory = true; }
			else if (argument == "--disk")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				raw.disk = *value; raw.usedDisk = true;
			}
			else if (argument == "--port" || argument == "--cart")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				// <port>=<file>: the port is a number, and the file is everything after the first '='
				const std::string_view text = *value;
				const usize equals = text.find('=');
				unsigned port = 0;
				bool numeric = equals != std::string_view::npos && equals > 0 && equals <= 3;
				for (usize i = 0; numeric && i < equals; ++i)
				{
					numeric = text[i] >= '0' && text[i] <= '9';
					port = port * 10 + static_cast<unsigned>(text[i] - '0');
				}
				if (!numeric || equals + 1 >= text.size())
					return std::unexpected(ParseError{ "'" + std::string(argument) + "' takes <port>=<file>, for example " + std::string(argument) + " 0=stick.img" });
				raw.ports.push_back(PortAttachment{ port, std::string(text.substr(equals + 1)), argument == "--cart" });
				raw.usedDisk = true;
			}
			else if (argument == "--window") { raw.window = true; raw.usedWindow = true; }
			else if (argument == "--terminal") { raw.terminal = true; raw.usedTerminal = true; }
			else if (argument == "--strict-mmio") raw.strictMmio = true;
			else if (argument == "--rtc")
			{
				auto value = nextValue(argument);
				if (!value) return std::unexpected(value.error());
				auto rtc = parseRtc(*value);
				if (!rtc) return std::unexpected(rtc.error());
				raw.rtc = *rtc;
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
		if (raw.usedSymbolTable && command != "link")
			return std::unexpected(invalidOption("--symtab", command));
		if (raw.usedGcSections && command != "link")
			return std::unexpected(invalidOption("--gc-sections", command));
		if ((raw.usedDashDash || raw.usedEnv || raw.usedHostDir || raw.strictMmio || raw.rtc) && command != "run")
			return std::unexpected(invalidOption(raw.usedEnv ? "--env" : raw.usedHostDir ? "--host-dir" : raw.strictMmio ? "--strict-mmio" :
				raw.rtc ? "--rtc" : "--", command));
		if (command == "asm")
		{
			if (raw.usedMemory || raw.usedDisk || raw.usedWindow || raw.usedTerminal || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory)
				return std::unexpected(invalidOption("a supplied option", command));
			return AssembleCommand{ std::move(inputs), std::move(raw.output), raw.compileOnly, raw.listing,
				raw.json, raw.debugInfo, raw.debugJson };
		}
		if (command == "link")
		{
			if (raw.compileOnly || raw.usedListing || raw.usedJson || raw.usedMemory || raw.usedDisk || raw.usedWindow || raw.usedTerminal || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory)
				return std::unexpected(invalidOption("a supplied option", command));
			if (raw.output.empty()) return std::unexpected(ParseError{ "'ceres link' needs -o <output.cres>" });
			return LinkCommand{ std::move(inputs), std::move(raw.output), raw.debugInfo, raw.debugJson, raw.symbolTable, raw.gcSections };
		}
		if (command == "ar")
		{
			if (raw.compileOnly || raw.usedListing || raw.usedJson || raw.usedDebugInfo || raw.usedDebugJson || raw.usedDisk ||
				raw.usedMemory || raw.usedWindow || raw.usedTerminal || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory || raw.usedOutput)
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
			if (raw.window && raw.terminal)
				return std::unexpected(ParseError{ "'--window' and '--terminal' are opposites: pick one" });
			return RunCommand{ std::move(inputs.front()), raw.memorySize, std::move(raw.disk), raw.listing, raw.debugInfo, raw.window, raw.terminal,
				std::move(raw.ports), std::move(raw.arguments), std::move(raw.environment), std::move(raw.hostDirectory), raw.strictMmio,
				raw.rtc };
		}
		if (command == "profile")
		{
			if (raw.compileOnly || raw.usedOutput || raw.usedJson || raw.usedDebugInfo || raw.usedDebugJson || raw.usedDisk || raw.usedWindow || raw.usedTerminal || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory)
				return std::unexpected(invalidOption("a supplied option", command));
			return ProfileCommand{ std::move(inputs.front()), raw.memorySize, raw.listing };
		}
		if (command == "disasm")
		{
			if (raw.compileOnly || raw.usedOutput || raw.usedListing || raw.usedJson || raw.usedMemory || raw.usedDisk || raw.usedWindow || raw.usedTerminal || raw.usedStopOnEntry || raw.usedServer || raw.usedHistory)
				return std::unexpected(invalidOption("a supplied option", command));
			return DisassembleCommand{ std::move(inputs.front()), raw.debugInfo, raw.debugJson };
		}
		if (raw.compileOnly || raw.usedOutput || raw.usedListing || raw.usedJson || raw.usedDebugInfo || raw.usedDebugJson || raw.usedDisk || raw.usedWindow || raw.usedTerminal)
			return std::unexpected(invalidOption("a supplied option", command));
		return DebugCommand{ std::move(inputs), raw.memorySize, raw.stopOnEntry, raw.server, raw.recordHistory };
	}
}

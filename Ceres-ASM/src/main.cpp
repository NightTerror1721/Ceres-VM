#include <cstdio>
#include <iostream>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "vm/ceresvm.h"
#include "vm/devices.h"
#include "vm/disassembler.h"
#include "assembler/assembler.h"
#include "debug/debug_info.h"
#include "debug/debug_cli.h"
#include "debug/debug_session.h"

namespace
{
	using namespace ceres;
	using namespace ceres::vm;

	constexpr std::string_view UsageText =
		"Ceres - assembler and virtual machine\n"
		"\n"
		"  ceres asm <source.casm> [<source2.casm> ...] [-o <output.cres>] [--listing] [--json]\n"
		"                          [--debug] [--emit-debug-json]\n"
		"      Assemble one or more source files into a single linked program.\n"
		"      Without -o the program is only checked.\n"
		"      --json prints diagnostics as a JSON array on stdout instead of\n"
		"      human-readable text on stderr, for editor tooling.\n"
		"      --debug records the line and symbol tables; with -o they are appended\n"
		"      to the .cres file. --emit-debug-json prints them on stdout as JSON and\n"
		"      implies --debug.\n"
		"\n"
		"  ceres run <file.casm|file.cres> [--memory <bytes>]\n"
		"      Run a program, assembling it first if given a source file.\n"
		"\n"
		"  ceres disasm <file.casm|file.cres> [--debug]\n"
		"      Print the text section as address, encoded word and instruction.\n"
		"      With --debug, annotated with the source file and line each word came from.\n"
		"\n"
		"  ceres debug <file.casm|file.cres> [<source2.casm> ...] [--memory <bytes>]\n"
		"                                    [--no-stop-on-entry]\n"
		"      Run a program under an interactive debugger: breakpoints, stepping by\n"
		"      source line, registers, memory and a reconstructed call stack.\n"
		"\n"
		"A bare path is shorthand for 'run'.\n";

	// Set by the assembling commands so a failure prints every diagnostic, not just the first.
	void reportAssemblyErrors(const casm::Assembler& assembler, std::span<const std::filesystem::path> paths)
	{
		std::cerr << "Failed to assemble";
		for (const auto& path : paths)
			std::cerr << ' ' << path.string();
		std::cerr << '\n';
		for (const auto& error : assembler.errors())
		{
			if (!error.file.empty())
				std::cerr << "  [" << error.file << ":" << error.line << "] " << error.message << '\n';
			else
				std::cerr << "  [line " << error.line << "] " << error.message << '\n';
		}
	}

	void reportAssemblyErrors(const casm::Assembler& assembler, const std::filesystem::path& path)
	{
		reportAssemblyErrors(assembler, std::span<const std::filesystem::path>(&path, 1));
	}

	// Minimal escaper for the fixed shape of message text the assembler produces; not a general
	// JSON serialiser.
	std::string jsonEscape(std::string_view text)
	{
		std::string out;
		out.reserve(text.size());
		for (char c : text)
		{
			switch (c)
			{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (static_cast<unsigned char>(c) < 0x20)
					{
						char buf[7];
						std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
						out += buf;
					}
					else
					{
						out += c;
					}
			}
		}
		return out;
	}

	// One JSON object per diagnostic, always valid JSON (an empty array on success) so editor
	// tooling can parse stdout unconditionally.
	void printJsonDiagnostics(const casm::Assembler& assembler)
	{
		std::cout << '[';
		bool first = true;
		for (const auto& error : assembler.errors())
		{
			if (!first)
				std::cout << ',';
			first = false;
			std::cout << "{\"file\":\"" << jsonEscape(error.file) << "\""
				<< ",\"line\":" << error.line
				<< ",\"column\":" << error.column
				<< ",\"severity\":\"error\""
				<< ",\"message\":\"" << jsonEscape(error.message) << "\"}";
		}
		std::cout << "]\n";
	}

	// A program together with whatever could be learned about where its instructions came from.
	// The debug information is empty unless it was asked for and actually available.
	struct LoadedProgram
	{
		Program program;
		debug::DebugInfo debugInfo;
	};

	// A .cres is loaded as-is; anything else is assembled first. Keeps every command able to take
	// either form without the caller having to care.
	std::optional<LoadedProgram> loadProgram(const std::filesystem::path& path, bool wantDebugInfo)
	{
		if (path.extension() == ".cres")
		{
			auto loaded = Program::loadFromFile(path);
			if (!loaded)
			{
				std::cerr << "Failed to load " << path.string() << ": " << loaded.error() << '\n';
				return std::nullopt;
			}

			debug::DebugInfo debugInfo;
			if (wantDebugInfo && loaded->hasDebugSection())
			{
				// A file whose debug section this build cannot read is still perfectly runnable,
				// so this is a warning rather than a failure to load.
				auto parsed = debug::DebugInfo::deserialize(loaded->debugSection());
				if (parsed.has_value())
					debugInfo = std::move(parsed.value());
				else
					std::cerr << "Ignoring debug section in " << path.string() << ": " << parsed.error() << '\n';
			}

			return LoadedProgram{ std::move(loaded.value()), std::move(debugInfo) };
		}

		casm::Assembler assembler{ casm::AssemblerOptions{ .emitDebugInfo = wantDebugInfo } };
		auto program = assembler.assemble({ path });

		if (!program.has_value() || assembler.hasErrors())
		{
			reportAssemblyErrors(assembler, path);
			return std::nullopt;
		}

		return LoadedProgram{ std::move(program.value()), assembler.debugInfo() };
	}

	void printListing(const Program& program, const std::filesystem::path& path, const debug::DebugInfo& debugInfo)
	{
		const ProgramHeader& header = program.header();
		std::cout << "; " << path.string()
			<< "  text=" << header.textSize
			<< "  rodata=" << header.rodataSize
			<< "  data=" << header.dataSize
			<< "  bss=" << header.bssSize
			<< "  entry=0x" << std::hex << header.entryPoint << std::dec << '\n';

		if (debugInfo.isEmpty())
		{
			std::cout << Disassembler::listing(program.text(), Memory::UnrestrictedSegmentStart);
			return;
		}

		// The same three columns as the plain listing, plus where each word came from. The location
		// shown is the expansion site, so a macro call reads as the one line the programmer wrote
		// rather than as a run of lines from somewhere inside the macro's body.
		const auto text = program.text();
		const usize count = text.size() / Instruction::Size;
		for (usize i = 0; i < count; ++i)
		{
			const usize offset = i * Instruction::Size;
			const u32 address = Memory::UnrestrictedSegmentStart.value() + static_cast<u32>(offset);
			const Instruction::RawType raw =
				static_cast<Instruction::RawType>(text[offset]) |
				(static_cast<Instruction::RawType>(text[offset + 1]) << 8) |
				(static_cast<Instruction::RawType>(text[offset + 2]) << 16) |
				(static_cast<Instruction::RawType>(text[offset + 3]) << 24);

			std::cout << std::format("{:08x}  {:08x}  {:<28}",
				address, raw, Disassembler::disassemble(Instruction(raw)));

			if (const auto location = debugInfo.locationOf(address); location.has_value())
			{
				std::cout << std::format("; {}:{}{}{}",
					std::filesystem::path(location->expansionFile).filename().string(),
					location->expansionLine,
					location->isMacroExpansion() ? " (macro)" : "",
					location->isPadding() ? " (padding)" : "");
			}

			std::cout << '\n';
		}

		if (const usize remainder = text.size() % Instruction::Size; remainder != 0)
			std::cout << std::format("{:08x}  <{} trailing byte(s)>\n",
				Memory::UnrestrictedSegmentStart.value() + static_cast<u32>(count * Instruction::Size), remainder);
	}

	int runProgram(const Program& program, usize memorySize)
	{
		CeresVM vm{ memorySize };

		SystemControlDevice systemControl{
			[&vm]() { vm.shutdown(); },
			[&vm]() { vm.shutdown(); }
		};
		systemControl.attachTo(vm.io());

		// Heap-allocated and deliberately never freed: the background reader thread below
		// stays blocked in a stdin read for as long as the process lives, so the device it
		// writes into must outlive this function's return rather than being torn down by
		// the CLI process's own shutdown.
		TerminalDevice& terminal = *new TerminalDevice();
		terminal.attachTo(vm.io());

		// The engine's step loop is single-threaded and can't poll stdin without blocking
		// the whole VM, so a separate thread feeds keystrokes into TerminalDevice's own
		// thread-safe ring buffer (pushInput) as they arrive. Input is line-buffered by the
		// host terminal, same as any shell command: a program reading port 0x02 sees nothing
		// until the user presses Enter, then the whole line (including '\n') at once.
		std::thread([&terminal]()
		{
			char c;
			while (std::cin.get(c))
				terminal.pushInput(c);
		}).detach();

		// Gives the machine a clock and, with it, the only asynchronous interrupt source it
		// has. Disarmed until a program writes to the command port.
		TimerDevice timer{};
		timer.attachTo(vm.io());

		if (auto loaded = vm.loadProgram(program); !loaded)
		{
			std::cerr << "Failed to load program: " << loaded.error() << '\n';
			return 1;
		}

		if (auto ran = vm.run(); !ran)
		{
			std::cerr << "Failed to run program: " << ran.error() << '\n';
			return 1;
		}

		return 0;
	}

	struct Options
	{
		std::string_view command;
		// 'asm' may take several; 'run' and 'disasm' always resolve to exactly one entry.
		std::vector<std::filesystem::path> inputs;
		std::filesystem::path output;
		bool listing = false;
		bool json = false;
		bool debugInfo = false;
		bool debugJson = false;
		bool stopOnEntry = true;
		usize memorySize = Memory::DefaultSize;
	};

	// Returns nullopt when the arguments do not describe a runnable command; the caller prints
	// the usage text.
	std::optional<Options> parseArguments(int argc, char** argv)
	{
		Options options;
		std::vector<std::string_view> positional;

		for (int i = 1; i < argc; ++i)
		{
			const std::string_view argument = argv[i];

			if (argument == "-o" || argument == "--output")
			{
				if (++i >= argc)
				{
					std::cerr << "Missing path after " << argument << '\n';
					return std::nullopt;
				}
				options.output = argv[i];
			}
			else if (argument == "--listing")
			{
				options.listing = true;
			}
			else if (argument == "--json")
			{
				options.json = true;
			}
			else if (argument == "--debug")
			{
				options.debugInfo = true;
			}
			else if (argument == "--emit-debug-json")
			{
				// Asking to see the tables is asking for them to be built.
				options.debugJson = true;
				options.debugInfo = true;
			}
			else if (argument == "--no-stop-on-entry")
			{
				options.stopOnEntry = false;
			}
			else if (argument == "--memory")
			{
				if (++i >= argc)
				{
					std::cerr << "Missing size after --memory\n";
					return std::nullopt;
				}
				options.memorySize = static_cast<usize>(std::stoull(argv[i]));
			}
			else if (argument == "-h" || argument == "--help")
			{
				return std::nullopt;
			}
			else if (argument.starts_with("-"))
			{
				std::cerr << "Unknown option '" << argument << "'\n";
				return std::nullopt;
			}
			else
			{
				positional.push_back(argument);
			}
		}

		if (positional.empty())
			return std::nullopt;

		// A bare path means 'run', so the common case stays short.
		if (positional[0] == "asm" || positional[0] == "run" || positional[0] == "disasm" || positional[0] == "debug")
		{
			options.command = positional[0];
			if (positional.size() < 2)
			{
				std::cerr << "Missing input file for '" << options.command << "'\n";
				return std::nullopt;
			}

			for (usize i = 1; i < positional.size(); ++i)
				options.inputs.emplace_back(positional[i]);

			// 'asm' and 'debug' link several sources into one program; 'run'/'disasm' need one.
			if (options.command != "asm" && options.command != "debug" && options.inputs.size() > 1)
			{
				std::cerr << "'" << options.command << "' takes a single input file\n";
				return std::nullopt;
			}
		}
		else
		{
			options.command = "run";
			options.inputs.emplace_back(positional[0]);
		}

		return options;
	}
}

int main(int argc, char** argv)
{
	using namespace ceres;
	using namespace ceres::vm;

	const auto optionsOpt = parseArguments(argc, argv);
	if (!optionsOpt.has_value())
	{
		std::cerr << UsageText;
		return 2;
	}

	const Options& options = optionsOpt.value();

	for (const auto& input : options.inputs)
	{
		if (!std::filesystem::exists(input))
		{
			std::cerr << "No such file: " << input.string() << '\n';
			return 1;
		}
	}

	if (options.command == "asm")
	{
		casm::Assembler assembler{ casm::AssemblerOptions{ .emitDebugInfo = options.debugInfo } };
		auto program = assembler.assemble(options.inputs);
		const bool failed = !program.has_value() || assembler.hasErrors();

		if (options.json)
			printJsonDiagnostics(assembler);
		else if (failed)
			reportAssemblyErrors(assembler, std::span<const std::filesystem::path>(options.inputs));

		if (failed)
			return 1;

		if (options.listing)
			printListing(program.value(), options.inputs.front(), assembler.debugInfo());

		if (options.debugJson)
			std::cout << assembler.debugInfo().toJson() << '\n';

		if (!options.output.empty())
		{
			if (auto saved = program->saveToFile(options.output); !saved)
			{
				std::cerr << "Failed to write " << options.output.string() << ": " << saved.error() << '\n';
				return 1;
			}
			std::cerr << "Wrote " << options.output.string() << '\n';
		}

		return 0;
	}

	if (options.command == "debug")
	{
		auto session = debug::DebugSession::launch(debug::LaunchConfig{
			.sources = options.inputs,
			.memorySize = options.memorySize,
			.stopOnEntry = options.stopOnEntry
		});

		if (!session.has_value())
		{
			std::cerr << session.error() << '\n';
			return 1;
		}

		debug::DebugCLI cli{ *session.value() };
		return cli.run();
	}

	if (options.command == "disasm")
	{
		auto loaded = loadProgram(options.inputs.front(), options.debugInfo);
		if (!loaded.has_value())
			return 1;

		printListing(loaded->program, options.inputs.front(), loaded->debugInfo);

		if (options.debugJson)
			std::cout << loaded->debugInfo.toJson() << '\n';

		return 0;
	}

	// run
	auto loaded = loadProgram(options.inputs.front(), options.debugInfo);
	if (!loaded.has_value())
		return 1;

	if (options.listing)
		printListing(loaded->program, options.inputs.front(), loaded->debugInfo);

	return runProgram(loaded->program, options.memorySize);
}

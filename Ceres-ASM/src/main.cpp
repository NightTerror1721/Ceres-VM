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

namespace
{
	using namespace ceres;
	using namespace ceres::vm;

	constexpr std::string_view UsageText =
		"Ceres - assembler and virtual machine\n"
		"\n"
		"  ceres asm <source.casm> [<source2.casm> ...] [-o <output.cres>] [--listing] [--json]\n"
		"      Assemble one or more source files into a single linked program.\n"
		"      Without -o the program is only checked.\n"
		"      --json prints diagnostics as a JSON array on stdout instead of\n"
		"      human-readable text on stderr, for editor tooling.\n"
		"\n"
		"  ceres run <file.casm|file.cres> [--memory <bytes>]\n"
		"      Run a program, assembling it first if given a source file.\n"
		"\n"
		"  ceres disasm <file.casm|file.cres>\n"
		"      Print the text section as address, encoded word and instruction.\n"
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

	std::optional<Program> assembleFile(const std::filesystem::path& path)
	{
		casm::Assembler assembler{};
		auto program = assembler.assemble({ path });

		if (!program.has_value() || assembler.hasErrors())
		{
			reportAssemblyErrors(assembler, path);
			return std::nullopt;
		}
		return program;
	}

	// A .cres is loaded as-is; anything else is assembled first. Keeps every command able to take
	// either form without the caller having to care.
	std::optional<Program> loadProgram(const std::filesystem::path& path)
	{
		if (path.extension() == ".cres")
		{
			auto loaded = Program::loadFromFile(path);
			if (!loaded)
			{
				std::cerr << "Failed to load " << path.string() << ": " << loaded.error() << '\n';
				return std::nullopt;
			}
			return std::move(loaded.value());
		}

		return assembleFile(path);
	}

	void printListing(const Program& program, const std::filesystem::path& path)
	{
		const ProgramHeader& header = program.header();
		std::cout << "; " << path.string()
			<< "  text=" << header.textSize
			<< "  rodata=" << header.rodataSize
			<< "  data=" << header.dataSize
			<< "  bss=" << header.bssSize
			<< "  entry=0x" << std::hex << header.entryPoint << std::dec << '\n';
		std::cout << Disassembler::listing(program.text(), Memory::UnrestrictedSegmentStart);
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
		if (positional[0] == "asm" || positional[0] == "run" || positional[0] == "disasm")
		{
			options.command = positional[0];
			if (positional.size() < 2)
			{
				std::cerr << "Missing input file for '" << options.command << "'\n";
				return std::nullopt;
			}

			for (usize i = 1; i < positional.size(); ++i)
				options.inputs.emplace_back(positional[i]);

			// Only 'asm' links several sources into one program; 'run'/'disasm' need one entry.
			if (options.command != "asm" && options.inputs.size() > 1)
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
		casm::Assembler assembler{};
		auto program = assembler.assemble(options.inputs);
		const bool failed = !program.has_value() || assembler.hasErrors();

		if (options.json)
			printJsonDiagnostics(assembler);
		else if (failed)
			reportAssemblyErrors(assembler, std::span<const std::filesystem::path>(options.inputs));

		if (failed)
			return 1;

		if (options.listing)
			printListing(program.value(), options.inputs.front());

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

	if (options.command == "disasm")
	{
		auto program = loadProgram(options.inputs.front());
		if (!program.has_value())
			return 1;

		printListing(program.value(), options.inputs.front());
		return 0;
	}

	// run
	auto program = loadProgram(options.inputs.front());
	if (!program.has_value())
		return 1;

	if (options.listing)
		printListing(program.value(), options.inputs.front());

	return runProgram(program.value(), options.memorySize);
}

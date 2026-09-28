#include <ceres/driver/driver.h>
#include "machine_runner.h"

#include <ceres/asm/assembler.h>
#include <ceres/asm/object_linker.h>
#include <ceres/core/format/debug_info.h>
#include <ceres/core/isa/disassembler.h>
#include <ceres/debug/debug_cli.h>
#include <ceres/debug/debug_server.h>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <format>
#include <span>

namespace ceres::driver
{
	using namespace casm;
	using namespace debug;
	using namespace fmt;
	using namespace isa;
	using namespace vm;

	namespace
	{
		bool inputsExist(std::span<const std::filesystem::path> paths, std::ostream& err)
		{
			for (const auto& path : paths)
				if (!std::filesystem::exists(path)) { err << "No such file: " << path.string() << '\n'; return false; }
			return true;
		}

		void reportErrors(const Assembler& assembler, std::span<const std::filesystem::path> paths, std::ostream& err)
		{
			if (assembler.hasErrors())
			{
				err << "Failed to assemble";
				for (const auto& path : paths) err << ' ' << path.string();
				err << '\n';
			}
			for (const auto& error : assembler.errors())
				err << (error.isWarning() ? "  warning " : "  ") << '[' << error.file << ':' << error.line << "] " << error.message << '\n';
		}

		std::string jsonEscape(std::string_view text)
		{
			std::string out;
			out.reserve(text.size());
			for (const char c : text)
			{
				switch (c)
				{
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					default: out += c; break;
				}
			}
			return out;
		}

		void printJsonErrors(const Assembler& assembler, std::ostream& out)
		{
			out << '[';
			bool first = true;
			for (const auto& error : assembler.errors())
			{
				if (!first) out << ',';
				first = false;
				out << "{\"file\":\"" << jsonEscape(error.file) << "\",\"line\":" << error.line
					<< ",\"column\":" << error.column << ",\"severity\":\""
					<< (error.isWarning() ? "warning" : "error") << "\",\"message\":\""
					<< jsonEscape(error.message) << "\"}";
			}
			out << "]\n";
		}

		struct LoadedProgram { Program program; DebugInfo debugInfo; };

		std::optional<LoadedProgram> loadProgram(const std::filesystem::path& path, bool wantDebugInfo, std::ostream& err)
		{
			if (path.extension() == ".cres")
			{
				auto loaded = Program::loadFromFile(path);
				if (!loaded) { err << "Failed to load " << path.string() << ": " << loaded.error() << '\n'; return std::nullopt; }
				DebugInfo debugInfo;
				if (wantDebugInfo && loaded->hasDebugSection())
				{
					auto parsed = DebugInfo::deserialize(loaded->debugSection());
					if (parsed) debugInfo = std::move(*parsed);
					else err << "Ignoring debug section in " << path.string() << ": " << parsed.error() << '\n';
				}
				return LoadedProgram{std::move(*loaded), std::move(debugInfo)};
			}

			Assembler assembler{AssemblerOptions{.emitDebugInfo = wantDebugInfo}};
			auto program = assembler.assemble({path});
			if (!program || assembler.hasErrors()) { reportErrors(assembler, std::span(&path, 1), err); return std::nullopt; }
			return LoadedProgram{std::move(*program), assembler.debugInfo()};
		}

		void printListing(const Program& program, const std::filesystem::path& path, const DebugInfo& debugInfo, std::ostream& out)
		{
			const auto& header = program.header();
			out << "; " << path.string() << "  text=" << header.textSize << "  rodata=" << header.rodataSize
				<< "  data=" << header.dataSize << "  bss=" << header.bssSize << "  entry=0x" << std::hex
				<< header.entryPoint << std::dec << '\n';
			if (debugInfo.isEmpty()) { out << Disassembler::listing(program.text(), Memory::UnrestrictedSegmentStart); return; }
			const auto text = program.text();
			for (usize i = 0; i < text.size() / Instruction::Size; ++i)
			{
				const usize offset = i * Instruction::Size;
				const u32 address = Memory::UnrestrictedSegmentStart.value() + static_cast<u32>(offset);
				const Instruction::RawType raw = static_cast<Instruction::RawType>(text[offset]) |
					(static_cast<Instruction::RawType>(text[offset + 1]) << 8) |
					(static_cast<Instruction::RawType>(text[offset + 2]) << 16) |
					(static_cast<Instruction::RawType>(text[offset + 3]) << 24);
				out << std::format("{:08x}  {:08x}  {:<28}", address, raw, Disassembler::disassemble(Instruction(raw)));
				if (const auto location = debugInfo.locationOf(address))
					out << std::format("; {}:{}{}{}", std::filesystem::path(location->expansionFile).filename().string(), location->expansionLine, location->isMacroExpansion() ? " (macro)" : "", location->isPadding() ? " (padding)" : "");
				out << '\n';
			}
		}

		int executeAssemble(const AssembleCommand& command, std::ostream& out, std::ostream& err)
		{
			if (!inputsExist(command.inputs, err)) return 1;
			if (command.compileOnly)
			{
				if (command.inputs.size() != 1) { err << "'ceres asm -c' takes a single source file\n"; return 2; }
				Assembler assembler{AssemblerOptions{.emitDebugInfo = command.debugInfo, .requireEntryPoint = false}};
				auto object = assembler.assembleObject(command.inputs.front());
				const bool failed = !object || assembler.hasErrors();
				if (command.jsonDiagnostics) printJsonErrors(assembler, out);
				else if (failed || !assembler.errors().empty()) reportErrors(assembler, command.inputs, err);
				if (failed || command.output.empty()) return failed ? 1 : 0;
				if (auto saved = object->write(command.output); !saved) { err << saved.error() << '\n'; return 1; }
				err << "Wrote " << command.output.string() << '\n';
				return 0;
			}

			Assembler assembler{AssemblerOptions{.emitDebugInfo = command.debugInfo, .requireEntryPoint = !command.output.empty()}};
			auto program = assembler.assemble(command.inputs);
			const bool failed = !program || assembler.hasErrors();
			if (command.jsonDiagnostics) printJsonErrors(assembler, out);
			else if (failed || !assembler.errors().empty()) reportErrors(assembler, command.inputs, err);
			if (failed) return 1;
			if (command.debugJson) out << assembler.debugInfo().toJson() << '\n';
			if (command.listing) printListing(*program, command.inputs.front(), assembler.debugInfo(), out);
			if (command.output.empty()) return 0;
			if (auto saved = program->saveToFile(command.output); !saved) { err << "Failed to write " << command.output.string() << ": " << saved.error() << '\n'; return 1; }
			err << "Wrote " << command.output.string() << '\n';
			return 0;
		}

		int executeLink(const LinkCommand& command, std::ostream& out, std::ostream& err)
		{
			if (!inputsExist(command.inputs, err)) return 1;
			std::vector<ObjectArchive::Member> inputs;
			for (const auto& path : command.inputs)
			{
				auto members = readObjectsFrom(path);
				if (!members) { err << members.error() << '\n'; return 1; }
				for (auto& member : *members) inputs.push_back(std::move(member));
			}
			ObjectLinker linker;
			auto program = linker.link(std::move(inputs), {.requireEntryPoint = true, .emitDebugInfo = command.debugInfo,
				.emitSymbolTable = command.symbolTable, .gcSections = command.gcSections});
			if (!program) { for (const auto& error : linker.errors()) err << "Link error: " << error << '\n'; return 1; }
			if (auto saved = program->saveToFile(command.output); !saved) { err << "Failed to write " << command.output.string() << ": " << saved.error() << '\n'; return 1; }
			err << "Wrote " << command.output.string() << '\n';
			if (command.debugJson) out << linker.takeDebugInfo().toJson() << '\n';
			return 0;
		}

		int executeArchive(const ArchiveCommand& command, std::ostream& err)
		{
			if (!inputsExist(command.inputs, err)) return 1;
			ObjectArchive archive;
			for (const auto& path : command.inputs)
			{
				auto members = readObjectsFrom(path);
				if (!members) { err << members.error() << '\n'; return 1; }
				for (auto& member : *members) { member.fromArchive = false; archive.members.push_back(std::move(member)); }
			}
			if (auto saved = archive.write(command.output); !saved) { err << saved.error() << '\n'; return 1; }
			err << "Wrote " << command.output.string() << " (" << archive.members.size() << " objects)\n";
			return 0;
		}

		int executeDebug(const DebugCommand& command, std::ostream& err)
		{
			if (!inputsExist(command.inputs, err)) return 1;
			auto session = DebugSession::launch({.sources = command.inputs, .memorySize = command.machine.ramBytes, .vramSize = command.machine.vramBytes,
				.cpuClockHz = command.machine.cpuClockHz, .profileId = static_cast<u32>(command.machine.id),
				.gpu = { .gpuClockHz = command.machine.gpuClockHz, .maxLevel = command.machine.maxVideo, .maxWidth = command.machine.maxWidth,
					.maxHeight = command.machine.maxHeight, .refresh = 60 },
				.stopOnEntry = command.stopOnEntry,
				.recordHistory = command.recordHistory});
			if (!session) { err << session.error() << '\n'; return 1; }
			if (command.server) { DebugServer server{**session}; return server.run(); }
			DebugCLI cli{**session};
			return cli.run();
		}

		// --headless, or CERES_HEADLESS set to something other than "", "0" or "false": scripts and tests set the
		// variable so that no window opens. An explicit --window still wins.
		bool headlessRequested(const RunCommand& command)
		{
			if (command.headless)
				return true;
			const char* value = std::getenv("CERES_HEADLESS");
			if (value == nullptr)
				return false;
			const std::string_view text = value;
			return !text.empty() && text != "0" && text != "false";
		}

		// The shell of `ceres run` without a program, or with --shell: <sysroot>/bin/shell.cres, the sysroot from
		// --sysroot or CERES_SYSROOT. Null, with the reason told, when there is none.
		std::shared_ptr<const ShellProgram> findShell(const RunCommand& command, std::ostream& err)
		{
			std::filesystem::path sysroot = command.sysroot;
			if (sysroot.empty())
				if (const char* value = std::getenv("CERES_SYSROOT"); value != nullptr)
					sysroot = value;
			const std::string_view why = command.input.empty() ? "No program was given" : "--shell";
			if (sysroot.empty())
			{
				err << why << ", and the shell is in <sysroot>/bin/shell.cres: pass --sysroot <dir> or set CERES_SYSROOT "
					"(the STDLIB installs it: make install PREFIX=<dir>).\n";
				return nullptr;
			}
			const std::filesystem::path path = sysroot / "bin" / "shell.cres";
			if (!std::filesystem::is_regular_file(path))
			{
				err << why << ", and there is no shell at " << path.string() << " (the STDLIB installs it: make install PREFIX=<dir>).\n";
				return nullptr;
			}
			auto program = Program::loadFromFile(path);
			if (!program)
			{
				err << "Failed to load the shell " << path.string() << ": " << program.error() << '\n';
				return nullptr;
			}
			return std::make_shared<const ShellProgram>(ShellProgram{ std::move(*program), path.string() });
		}

		int executeRun(const RunCommand& command, HostServices services, const WindowHostFactory& windowHost)
		{
			// A window asked for without a windowed host to provide one is reported before any
			// work is done: assembling the program would only waste time on the way to the same error.
			if (command.window && !windowHost)
			{
				*services.diagnostics << "This build has no windowed host: rebuild with CERES_ENABLE_SDL to use 'run --window'.\n";
				return 1;
			}

			// The shell (plan/v2 F7): the program when none is given, and where --shell goes back to.
			std::shared_ptr<const ShellProgram> shell;
			if (command.input.empty() || command.shell)
			{
				shell = findShell(command, *services.diagnostics);
				if (!shell) return 1;
			}
			std::optional<LoadedProgram> loaded;
			if (!command.input.empty())
			{
				if (!inputsExist(std::span(&command.input, 1), *services.diagnostics)) return 1;
				loaded = loadProgram(command.input, command.debugInfo, *services.diagnostics);
				if (!loaded) return 1;
				if (command.listing) printListing(loaded->program, command.input, loaded->debugInfo, *services.output);
			}
			const Program& first = loaded ? loaded->program : shell->program;
			const std::string firstPath = loaded ? command.input.string() : shell->path;
			// With the shell, the host directory is the current one unless --host-dir says otherwise: its commands
			// look at files, and run programs from there.
			std::filesystem::path hostDirectory = command.hostDirectory;
			if (shell && hostDirectory.empty())
			{
				std::error_code error;
				hostDirectory = std::filesystem::current_path(error);
			}

			WindowHost host;
			if (command.window)
			{
				// Asked for by name: open it now, and say so if that cannot be done.
				try
				{
					host = windowHost();
				}
				catch (const std::exception& error)
				{
					*services.diagnostics << "Failed to open a window: " << error.what() << '\n';
					return 1;
				}
			}
			else if (windowHost && !headlessRequested(command))
			{
				// A machine with a screen: its window opens when it starts. If there turns out to be no display, it
				// runs without one.
				try
				{
					host = windowHost();
				}
				catch (const std::exception&)
				{
					host = {};
				}
			}

			vm::ProgramArguments arguments{ { firstPath }, command.environment };
			arguments.arguments.insert(arguments.arguments.end(), command.arguments.begin(), command.arguments.end());
			return runMachine(first, command.machine, nullptr, command.diskImage, command.ports, std::move(arguments),
				hostDirectory, services, HostIo{ host.input.get(), host.video.get(), host.audio.get() },
				MachineOptions{ .strictMmio = command.strictMmio, .rtc = command.rtc, .speed = command.speed, .record = command.record,
					.replay = command.replay, .logFile = command.logFile, .refresh = command.refresh, .fullscreen = command.fullscreen,
					.exitOnHalt = command.exitOnHalt, .requireWindow = command.window, .framesDir = command.framesDir,
					.transcript = command.transcript, .screenLog = command.screenLog, .typeFile = command.typeFile, .keysFile = command.keysFile,
					.shell = shell, .startsInShell = !loaded });
		}

		int executeProfile(const ProfileCommand& command, HostServices services)
		{
			if (!inputsExist(std::span(&command.input, 1), *services.diagnostics)) return 1;
			auto loaded = loadProgram(command.input, true, *services.diagnostics);
			if (!loaded) return 1;
			if (command.listing) printListing(loaded->program, command.input, loaded->debugInfo, *services.output);
			if (loaded->debugInfo.lines().empty())
			{
				*services.diagnostics << "Cannot profile a .cres without debug information: assemble with --debug, or profile the source directly.\n";
				return 1;
			}
			return runMachine(loaded->program, command.machine, &loaded->debugInfo, {}, {},
				vm::ProgramArguments{ { command.input.string() }, {} }, {}, services);
		}

		int executeDisassemble(const DisassembleCommand& command, HostServices services)
		{
			if (!inputsExist(std::span(&command.input, 1), *services.diagnostics)) return 1;
			auto loaded = loadProgram(command.input, command.debugInfo, *services.diagnostics);
			if (!loaded) return 1;
			printListing(loaded->program, command.input, loaded->debugInfo, *services.output);
			if (command.debugJson) *services.output << loaded->debugInfo.toJson() << '\n';
			return 0;
		}
	}

	int execute(const Command& command, HostServices services, WindowHostFactory windowHost)
	{
		auto& out = *services.output;
		auto& err = *services.diagnostics;
		if (const auto* value = std::get_if<AssembleCommand>(&command)) return executeAssemble(*value, out, err);
		if (const auto* value = std::get_if<LinkCommand>(&command)) return executeLink(*value, out, err);
		if (const auto* value = std::get_if<ArchiveCommand>(&command)) return executeArchive(*value, err);
		if (const auto* value = std::get_if<DebugCommand>(&command)) return executeDebug(*value, err);
		if (const auto* value = std::get_if<RunCommand>(&command)) return executeRun(*value, services, windowHost);
		if (const auto* value = std::get_if<ProfileCommand>(&command)) return executeProfile(*value, services);
		return executeDisassemble(std::get<DisassembleCommand>(command), services);
	}

	int runCommandLine(int argc, char* const argv[], HostServices services, WindowHostFactory windowHost)
	{
		auto command = parseCommandLine(argc, argv);
		if (!command)
		{
			if (!command.error().message.empty()) *services.diagnostics << command.error().message << '\n';
			*services.diagnostics << usageText();
			return 2;
		}
		return execute(*command, services, std::move(windowHost));
	}
}

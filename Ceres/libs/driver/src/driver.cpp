#include <ceres/driver/driver.h>
#include "machine_runner.h"

#include <ceres/asm/assembler.h>
#include <ceres/asm/object_linker.h>
#include <ceres/core/format/debug_info.h>
#include <ceres/core/isa/disassembler.h>
#include <ceres/debug/debug_cli.h>
#include <ceres/debug/debug_server.h>
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
			auto program = linker.link(std::move(inputs), {.requireEntryPoint = true, .emitDebugInfo = command.debugInfo});
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
			auto session = DebugSession::launch({.sources = command.inputs, .memorySize = command.memorySize, .stopOnEntry = command.stopOnEntry, .recordHistory = command.recordHistory});
			if (!session) { err << session.error() << '\n'; return 1; }
			if (command.server) { DebugServer server{**session}; return server.run(); }
			DebugCLI cli{**session};
			return cli.run();
		}

		int executeRun(const RunCommand& command, HostServices services)
		{
			if (!inputsExist(std::span(&command.input, 1), *services.diagnostics)) return 1;
			auto loaded = loadProgram(command.input, command.debugInfo, *services.diagnostics);
			if (!loaded) return 1;
			if (command.listing) printListing(loaded->program, command.input, loaded->debugInfo, *services.output);
			return runMachine(loaded->program, command.memorySize, nullptr, command.diskImage, services);
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
			return runMachine(loaded->program, command.memorySize, &loaded->debugInfo, {}, services);
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

	int execute(const Command& command, HostServices services)
	{
		auto& out = *services.output;
		auto& err = *services.diagnostics;
		if (const auto* value = std::get_if<AssembleCommand>(&command)) return executeAssemble(*value, out, err);
		if (const auto* value = std::get_if<LinkCommand>(&command)) return executeLink(*value, out, err);
		if (const auto* value = std::get_if<ArchiveCommand>(&command)) return executeArchive(*value, err);
		if (const auto* value = std::get_if<DebugCommand>(&command)) return executeDebug(*value, err);
		if (const auto* value = std::get_if<RunCommand>(&command)) return executeRun(*value, services);
		if (const auto* value = std::get_if<ProfileCommand>(&command)) return executeProfile(*value, services);
		return executeDisassemble(std::get<DisassembleCommand>(command), services);
	}

	int runCommandLine(int argc, char* const argv[], HostServices services)
	{
		auto command = parseCommandLine(argc, argv);
		if (!command)
		{
			if (!command.error().message.empty()) *services.diagnostics << command.error().message << '\n';
			*services.diagnostics << usageText();
			return 2;
		}
		return execute(*command, services);
	}
}

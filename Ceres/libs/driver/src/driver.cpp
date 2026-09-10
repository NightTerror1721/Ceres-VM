#include <ceres/driver/driver.h>

#include <ceres/asm/assembler.h>
#include <ceres/asm/object_linker.h>
#include <ceres/debug/debug_cli.h>
#include <ceres/debug/debug_server.h>
#include <ceres/devices/devices.h>
#include <ceres/devices/storage_devices.h>
#include <ceres/vm/ceresvm.h>
#include <iostream>
#include <span>

namespace ceres::driver
{
	using namespace casm;
	using namespace debug;
	using namespace devices;
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

		int executeAssemble(const AssembleCommand& command, std::ostream& out, std::ostream& err)
		{
			if (!inputsExist(command.inputs, err)) return 1;
			if (command.compileOnly)
			{
				if (command.inputs.size() != 1) { err << "'ceres asm -c' takes a single source file\n"; return 2; }
				Assembler assembler{AssemblerOptions{.emitDebugInfo = command.debugInfo, .requireEntryPoint = false}};
				auto object = assembler.assembleObject(command.inputs.front());
				const bool failed = !object || assembler.hasErrors();
				if (failed || !assembler.errors().empty()) reportErrors(assembler, command.inputs, err);
				if (failed || command.output.empty()) return failed ? 1 : 0;
				if (auto saved = object->write(command.output); !saved) { err << saved.error() << '\n'; return 1; }
				err << "Wrote " << command.output.string() << '\n';
				return 0;
			}

			Assembler assembler{AssemblerOptions{.emitDebugInfo = command.debugInfo, .requireEntryPoint = !command.output.empty()}};
			auto program = assembler.assemble(command.inputs);
			const bool failed = !program || assembler.hasErrors();
			if (failed || !assembler.errors().empty()) reportErrors(assembler, command.inputs, err);
			if (failed) return 1;
			if (command.debugJson) out << assembler.debugInfo().toJson() << '\n';
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
	}

	int execute(const Command& command, HostServices services)
	{
		auto& out = *services.output;
		auto& err = *services.diagnostics;
		if (const auto* value = std::get_if<AssembleCommand>(&command)) return executeAssemble(*value, out, err);
		if (const auto* value = std::get_if<LinkCommand>(&command)) return executeLink(*value, out, err);
		if (const auto* value = std::get_if<ArchiveCommand>(&command)) return executeArchive(*value, err);
		if (const auto* value = std::get_if<DebugCommand>(&command)) return executeDebug(*value, err);
		err << "This command is not yet available through the driver.\n";
		return 1;
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

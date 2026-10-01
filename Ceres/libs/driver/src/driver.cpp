#include <ceres/driver/driver.h>
#include "machine_runner.h"

#include <ceres/asm/assembler.h>
#include <ceres/asm/object_linker.h>
#include <ceres/core/format/debug_info.h>
#include <ceres/core/isa/disassembler.h>
#include <ceres/debug/debug_cli.h>
#include <ceres/debug/debug_server.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <format>
#include <span>
#include <system_error>

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

		// Defined below; loadProgram reaches it when it has already-built objects to link (the standard
		// library), which assemble() alone cannot consume.
		std::optional<LoadedProgram> assembleAndLink(const std::vector<std::filesystem::path>& sources,
			std::span<const std::filesystem::path> objects, bool emitDebugInfo, bool requireEntryPoint,
			const std::vector<std::filesystem::path>& importDirectories, bool jsonDiagnostics,
			std::ostream& out, std::ostream& err);

		std::optional<LoadedProgram> loadProgram(const std::filesystem::path& path, bool wantDebugInfo,
			const std::vector<std::filesystem::path>& importDirectories, std::span<const std::filesystem::path> archives,
			std::ostream& err)
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

			// An archive to link (the standard library) forces the object pipeline: the in-memory
			// linker only sees sources, and a name the program calls can live in an already-built
			// object.
			if (!archives.empty())
				return assembleAndLink({path}, archives, wantDebugInfo, true, importDirectories, false, err, err);

			Assembler assembler{AssemblerOptions{.emitDebugInfo = wantDebugInfo, .importDirectories = importDirectories}};
			auto program = assembler.assemble({path});
			if (!program || assembler.hasErrors()) { reportErrors(assembler, std::span(&path, 1), err); return std::nullopt; }
			return LoadedProgram{std::move(*program), assembler.debugInfo()};
		}

		// The standard library's lib directory, where --stdlib looks for it: stdlib/lib under the first
		// install directory that has one (CERES_PATH, then the one ceres is in). Empty when none does.
		std::filesystem::path findStdlibLib(std::span<const std::filesystem::path> installDirectories)
		{
			for (const auto& directory : installDirectories)
			{
				std::error_code error;
				const auto lib = directory / "stdlib" / "lib";
				if (std::filesystem::is_directory(lib, error))
					return lib;
			}
			return {};
		}

		// Where imports are looked for, and the standard library's lib when --stdlib found it. The
		// lib is resolved here once, so the archive's path is derived from the same lookup instead of
		// probing the filesystem a second time and risking a different answer.
		struct ImportDirectories
		{
			std::vector<std::filesystem::path> directories;   // the command's -I, then --stdlib's lib
			std::filesystem::path stdlibLib;                  // empty unless --stdlib found it
		};

		std::expected<ImportDirectories, std::string> resolveImportDirectories(
			std::span<const std::filesystem::path> own, bool stdlib, std::span<const std::filesystem::path> installDirectories)
		{
			ImportDirectories resolved;
			resolved.directories.assign(own.begin(), own.end());
			if (stdlib)
			{
				resolved.stdlibLib = findStdlibLib(installDirectories);
				if (resolved.stdlibLib.empty())
					return std::unexpected("ceres: --stdlib: cannot find the standard library (stdlib/lib in the directory Ceres is installed in)");
				resolved.directories.push_back(resolved.stdlibLib);
			}
			return resolved;
		}

		// -l <name>: lib<name>.car or lib<name>.cobj, looked for in each search directory (the
		// command's -L first, then --stdlib's lib when it was asked for). An archive is preferred
		// over a lone object, the way a linker prefers the library.
		std::expected<std::vector<std::filesystem::path>, std::string> resolveLibraries(
			std::span<const std::string> names, std::span<const std::filesystem::path> directories)
		{
			std::vector<std::filesystem::path> resolved;
			for (const auto& name : names)
			{
				std::filesystem::path library;
				std::error_code error;
				for (const auto& directory : directories)
				{
					for (const char* extension : { ".car", ".cobj" })
					{
						const auto candidate = directory / ("lib" + name + extension);
						if (std::filesystem::is_regular_file(candidate, error))
						{
							library = candidate;
							break;
						}
					}
					if (!library.empty())
						break;
				}
				if (library.empty())
					return std::unexpected("ceres: cannot find library 'lib" + name + ".car' or 'lib" + name + ".cobj' in the library search directories (-L, and --stdlib's lib)");
				resolved.push_back(library.lexically_normal());
			}
			return resolved;
		}

		// What a command that assembles from source needs: where imports are looked for, and the
		// archives to link. A .cres is already linked, so neither applies and --stdlib is ignored.
		struct SourceInputs
		{
			std::vector<std::filesystem::path> importDirectories;
			std::vector<std::filesystem::path> archives;
		};

		std::expected<SourceInputs, std::string> resolveSourceInputs(bool stdlib, const LibrarySearch& libraries,
			std::span<const std::filesystem::path> own, std::span<const std::filesystem::path> installDirectories)
		{
			auto resolved = resolveImportDirectories(own, stdlib, installDirectories);
			if (!resolved)
				return std::unexpected(resolved.error());

			SourceInputs inputs;
			inputs.importDirectories = std::move(resolved->directories);
			if (!resolved->stdlibLib.empty())
				inputs.archives.push_back(resolved->stdlibLib / "libceres.car");

			// -L first, then --stdlib's lib: the same order the C compiler uses. A library that
			// resolves to the standard archive already linked (say -lceres beside --stdlib) is not
			// linked a second time.
			std::vector<std::filesystem::path> searchDirectories(libraries.directories.begin(), libraries.directories.end());
			if (!resolved->stdlibLib.empty())
				searchDirectories.push_back(resolved->stdlibLib);
			auto extra = resolveLibraries(libraries.libraries, searchDirectories);
			if (!extra)
				return std::unexpected(extra.error());
			for (auto& library : *extra)
			{
				const bool alreadyLinked = std::any_of(inputs.archives.begin(), inputs.archives.end(),
					[&](const std::filesystem::path& archive) { return archive.lexically_normal() == library.lexically_normal(); });
				if (!alreadyLinked)
					inputs.archives.push_back(std::move(library));
			}
			return inputs;
		}

		// The same, for a command with a single input: a .cres is already linked, so --stdlib has
		// nothing to do for it.
		std::expected<SourceInputs, std::string> resolveSourceInputs(const std::filesystem::path& input, bool stdlib,
			const LibrarySearch& libraries, std::span<const std::filesystem::path> own, std::span<const std::filesystem::path> installDirectories)
		{
			if (input.extension() == ".cres")
			{
				// A .cres is already linked: --stdlib has nothing to add and is ignored. A -l is a
				// promise to link something, though, and dropping it silently would hide a mistake.
				if (!libraries.libraries.empty())
					return std::unexpected("ceres: -l links a program, and '" + input.string() + "' is already linked");
				return SourceInputs{};
			}
			return resolveSourceInputs(stdlib, libraries, own, installDirectories);
		}

		// The object pipeline: each source becomes one object on its own, then the objects - the
		// program's own and any already built (the standard library's libceres.car) - go to the object
		// linker. assemble() only ever sees sources, so a name that lives in a library has nothing to
		// resolve against until this. This is what `ceres link` does, driven from sources.
		std::optional<LoadedProgram> assembleAndLink(const std::vector<std::filesystem::path>& sources,
			std::span<const std::filesystem::path> objects, bool emitDebugInfo, bool requireEntryPoint,
			const std::vector<std::filesystem::path>& importDirectories, bool jsonDiagnostics,
			std::ostream& out, std::ostream& err)
		{
			std::vector<ObjectArchive::Member> inputs;
			Assembler assembler{AssemblerOptions{.emitDebugInfo = emitDebugInfo, .importDirectories = importDirectories}};
			for (const auto& source : sources)
			{
				auto object = assembler.assembleObject(source);
				const bool failed = !object || assembler.hasErrors();
				// A unit that assembled can still have warned (an unused private declaration), and
				// --json has to answer on a clean build - so report whether or not it failed, exactly
				// as the plain assembly path does.
				if (jsonDiagnostics) printJsonErrors(assembler, out);
				else if (failed || !assembler.errors().empty()) reportErrors(assembler, std::span(&source, 1), err);
				if (failed) return std::nullopt;
				inputs.push_back(ObjectArchive::Member{source.filename().string(), std::move(*object), false});
			}
			for (const auto& path : objects)
			{
				auto members = readObjectsFrom(path);
				if (!members) { err << members.error() << '\n'; return std::nullopt; }
				for (auto& member : *members)
					inputs.push_back(std::move(member));
			}
			ObjectLinker linker;
			auto program = linker.link(std::move(inputs), {.requireEntryPoint = requireEntryPoint, .emitDebugInfo = emitDebugInfo});
			if (!program) { for (const auto& error : linker.errors()) err << "Link error: " << error << '\n'; return std::nullopt; }
			LoadedProgram loaded{std::move(*program), {}};
			if (emitDebugInfo) loaded.debugInfo = linker.takeDebugInfo();
			return loaded;
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

		int executeAssemble(const AssembleCommand& command, std::span<const std::filesystem::path> installDirectories, std::ostream& out, std::ostream& err)
		{
			if (!inputsExist(command.inputs, err)) return 1;

			if (command.compileOnly)
			{
				if (command.inputs.size() != 1) { err << "'ceres asm -c' takes a single source file\n"; return 2; }
				auto directories = resolveImportDirectories(command.importDirectories, command.stdlib, installDirectories);
				if (!directories) { err << directories.error() << '\n'; return 1; }
				Assembler assembler{AssemblerOptions{.emitDebugInfo = command.debugInfo, .requireEntryPoint = false, .importDirectories = directories->directories}};
				auto object = assembler.assembleObject(command.inputs.front());
				const bool failed = !object || assembler.hasErrors();
				if (command.jsonDiagnostics) printJsonErrors(assembler, out);
				else if (failed || !assembler.errors().empty()) reportErrors(assembler, command.inputs, err);
				if (failed || command.output.empty()) return failed ? 1 : 0;
				if (auto saved = object->write(command.output); !saved) { err << saved.error() << '\n'; return 1; }
				err << "Wrote " << command.output.string() << '\n';
				return 0;
			}

			auto inputs = resolveSourceInputs(command.stdlib, command.libraries, command.importDirectories, installDirectories);
			if (!inputs) { err << inputs.error() << '\n'; return 1; }

			std::optional<LoadedProgram> loaded;
			if (!inputs->archives.empty())
			{
				loaded = assembleAndLink(command.inputs, inputs->archives, command.debugInfo,
					!command.output.empty(), inputs->importDirectories, command.jsonDiagnostics, out, err);
			}
			else
			{
				Assembler assembler{AssemblerOptions{.emitDebugInfo = command.debugInfo, .requireEntryPoint = !command.output.empty(), .importDirectories = inputs->importDirectories}};
				auto program = assembler.assemble(command.inputs);
				const bool failed = !program || assembler.hasErrors();
				if (command.jsonDiagnostics) printJsonErrors(assembler, out);
				else if (failed || !assembler.errors().empty()) reportErrors(assembler, command.inputs, err);
				if (failed) return 1;
				loaded = LoadedProgram{std::move(*program), assembler.debugInfo()};
			}
			if (!loaded) return 1;
			if (command.debugJson) out << loaded->debugInfo.toJson() << '\n';
			if (command.listing) printListing(loaded->program, command.inputs.front(), loaded->debugInfo, out);
			if (command.output.empty()) return 0;
			if (auto saved = loaded->program.saveToFile(command.output); !saved) { err << "Failed to write " << command.output.string() << ": " << saved.error() << '\n'; return 1; }
			err << "Wrote " << command.output.string() << '\n';
			return 0;
		}

		int executeLink(const LinkCommand& command, std::span<const std::filesystem::path> installDirectories, std::ostream& out, std::ostream& err)
		{
			std::vector<std::filesystem::path> paths(command.inputs);
			std::filesystem::path stdlibLib;
			if (command.stdlib)
			{
				stdlibLib = findStdlibLib(installDirectories);
				if (stdlibLib.empty())
				{
					err << "ceres: --stdlib: cannot find the standard library (stdlib/lib in the directory Ceres is installed in)\n";
					return 1;
				}
				paths.push_back(stdlibLib / "libceres.car");
			}

			// -l <name>: resolved through -L, then --stdlib's lib, and appended after the objects
			// named on the command line - the order a linker resolves them in.
			std::vector<std::filesystem::path> searchDirectories(command.libraries.directories.begin(), command.libraries.directories.end());
			if (!stdlibLib.empty())
				searchDirectories.push_back(stdlibLib);
			auto libraries = resolveLibraries(command.libraries.libraries, searchDirectories);
			if (!libraries) { err << libraries.error() << '\n'; return 1; }
			for (auto& library : *libraries)
			{
				const bool alreadyLinked = std::any_of(paths.begin(), paths.end(),
					[&](const std::filesystem::path& path) { return path.lexically_normal() == library.lexically_normal(); });
				if (!alreadyLinked)
					paths.push_back(std::move(library));
			}

			if (!inputsExist(paths, err)) return 1;
			std::vector<ObjectArchive::Member> inputs;
			for (const auto& path : paths)
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

		int executeDebug(const DebugCommand& command, std::span<const std::filesystem::path> installDirectories, std::ostream& err)
		{
			if (!inputsExist(command.inputs, err)) return 1;
			SourceInputs inputs;
			// A .cres is already linked, so --stdlib has nothing to do for it. The debugger takes
			// several sources, so only the single-.cres case is that.
			if (!(command.inputs.size() == 1 && command.inputs.front().extension() == ".cres"))
			{
				auto resolved = resolveSourceInputs(command.stdlib, command.libraries, command.importDirectories, installDirectories);
				if (!resolved) { err << resolved.error() << '\n'; return 1; }
				inputs = std::move(*resolved);
			}
			auto session = DebugSession::launch({.sources = command.inputs, .memorySize = command.machine.ramBytes, .vramSize = command.machine.vramBytes,
				.cpuClockHz = command.machine.cpuClockHz, .profileId = static_cast<u32>(command.machine.id),
				.gpu = { .gpuClockHz = command.machine.gpuClockHz, .maxLevel = command.machine.maxVideo, .maxWidth = command.machine.maxWidth,
					.maxHeight = command.machine.maxHeight, .refresh = 60, .spritesPerLine = command.machine.spritesPerLine,
					.vramInVblankOnly = command.machine.vramInVblankOnly },
				.stopOnEntry = command.stopOnEntry,
				.recordHistory = command.recordHistory,
				.importDirectories = std::move(inputs.importDirectories),
				.archives = std::move(inputs.archives)});
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

		// The shells an install directory can have, the whole one first: shell-small.cres is the same shell made for the
		// machines the other does not fit (micro's 64 KiB).
		constexpr std::array<std::string_view, 2> ShellNames{ "shell.cres", "shell-small.cres" };

		// The shell of `ceres run` without a program, or with --shell: the first of the install directories' shells
		// (CERES_PATH, then where ceres is; shell/shell.cres, then shell/shell-small.cres in each) that fits the
		// machine's RAM. Null, with the reason told, when none is there or none fits.
		std::shared_ptr<const ShellProgram> findShell(const RunCommand& command, std::span<const std::filesystem::path> directories, std::ostream& err)
		{
			const char* const why = command.input.empty() ? "No program was given" : "--shell";
			std::filesystem::path smallest;      // of the shells that do not fit, the one that needs least
			usize smallestNeeds = 0;
			for (const auto& directory : directories)
				for (const std::string_view name : ShellNames)
				{
					std::error_code error;
					const auto path = directory / "shell" / name;
					if (!std::filesystem::is_regular_file(path, error))
						continue;
					auto program = Program::loadFromFile(path);
					if (!program)
					{
						err << "Failed to load the shell " << path.string() << ": " << program.error() << '\n';
						return nullptr;
					}
					// Started as executeRun starts it when there is no program: its path, then the run's arguments.
					ProgramArguments arguments{ { path.string() }, command.environment };
					arguments.arguments.insert(arguments.arguments.end(), command.arguments.begin(), command.arguments.end());
					const usize needs = CeresVM::requiredMemory(program->header(), arguments);
					if (needs <= command.machine.ramBytes)
						return std::make_shared<const ShellProgram>(ShellProgram{ std::move(*program), path.string() });
					if (smallest.empty() || needs < smallestNeeds)
					{
						smallest = path;
						smallestNeeds = needs;
					}
				}
			if (!smallest.empty())
			{
				err << why << ", and no shell fits this machine's " << command.machine.ramBytes << " bytes of RAM: the smallest, "
					<< smallest.string() << ", needs " << smallestNeeds << ".\n";
				return nullptr;
			}
			err << why << ", and there is no shell: ";
			if (directories.empty())
				err << "nowhere to look for one.\n";
			else
			{
				err << "no shell/shell.cres or shell/shell-small.cres in ";
				for (std::size_t i = 0; i < directories.size(); ++i)
					err << (i == 0 ? "" : i + 1 == directories.size() ? " or " : ", ") << '\'' << directories[i].string() << '\'';
				err << " (CERES_PATH names the directory Ceres is installed in).\n";
			}
			return nullptr;
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
				shell = findShell(command, services.installDirectories, *services.diagnostics);
				if (!shell) return 1;
			}
			std::optional<LoadedProgram> loaded;
			if (!command.input.empty())
			{
				if (!inputsExist(std::span(&command.input, 1), *services.diagnostics)) return 1;
				const auto inputs = resolveSourceInputs(command.input, command.stdlib, command.libraries, command.importDirectories, services.installDirectories);
				if (!inputs) { *services.diagnostics << inputs.error() << '\n'; return 1; }
				loaded = loadProgram(command.input, command.debugInfo, inputs->importDirectories, inputs->archives, *services.diagnostics);
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
			const auto inputs = resolveSourceInputs(command.input, command.stdlib, command.libraries, command.importDirectories, services.installDirectories);
			if (!inputs) { *services.diagnostics << inputs.error() << '\n'; return 1; }
			auto loaded = loadProgram(command.input, true, inputs->importDirectories, inputs->archives, *services.diagnostics);
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
			const auto inputs = resolveSourceInputs(command.input, command.stdlib, command.libraries, command.importDirectories, services.installDirectories);
			if (!inputs) { *services.diagnostics << inputs.error() << '\n'; return 1; }
			auto loaded = loadProgram(command.input, command.debugInfo, inputs->importDirectories, inputs->archives, *services.diagnostics);
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
		if (const auto* value = std::get_if<AssembleCommand>(&command)) return executeAssemble(*value, services.installDirectories, out, err);
		if (const auto* value = std::get_if<LinkCommand>(&command)) return executeLink(*value, services.installDirectories, out, err);
		if (const auto* value = std::get_if<ArchiveCommand>(&command)) return executeArchive(*value, err);
		if (const auto* value = std::get_if<DebugCommand>(&command)) return executeDebug(*value, services.installDirectories, err);
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

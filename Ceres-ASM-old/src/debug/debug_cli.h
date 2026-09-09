#pragma once

// The interactive front end for `ceres debug`. A gdb-shaped REPL over a DebugSession, and the
// first debugging tool this project has had: everything before it could only print a static
// listing and hope.
//
// It reads its own commands from stdin, which is why a program's input is fed with an explicit
// `input` command rather than typed straight through — the two would otherwise be competing for
// the same keystrokes.

#include "debug_session.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ceres::debug
{
	class DebugCLI
	{
	private:
		DebugSession& _session;
		bool _quit = false;
		// Program output goes straight through to stdout; this tracks whether it ended mid-line
		// so the prompt does not land in the middle of the program's own text.
		bool _outputAtLineStart = true;
		// Source text, read lazily and kept: `list` is the one command likely to be used over and
		// over on the same file.
		std::unordered_map<std::string, std::vector<std::string>> _sourceCache;
		// What a bare `list` shows next, and what a bare `x` dumps next.
		u32 _listCentre = 0;
		u32 _nextDumpAddress = 0;

	public:
		DebugCLI() = delete;
		DebugCLI(const DebugCLI&) = delete;
		DebugCLI(DebugCLI&&) = delete;
		~DebugCLI() = default;

		DebugCLI& operator=(const DebugCLI&) = delete;
		DebugCLI& operator=(DebugCLI&&) = delete;

		explicit DebugCLI(DebugSession& session) : _session(session) {}

	public:
		// Runs until the user quits or the program ends. Returns the process exit code.
		int run();

	private:
		void printBanner() const;
		void printHelp() const;
		void printStop(const StopEvent& event);
		void printSourceContext(u32 around, u32 radius);
		void printRegisters() const;
		void printBacktrace() const;
		void printBreakpoints() const;
		void printDisassembly(u32 around, u32 count);
		void printGlobals() const;
		void dumpMemory(u32 address, u32 count);

		bool execute(const std::string& line);

		// Accepts a decimal or 0x-prefixed number, a symbol name, or file:line - whatever the
		// command in hand can make sense of. Empty when it means none of them.
		std::optional<u32> resolveAddress(std::string_view text) const;
		std::optional<u32> parseNumber(std::string_view text) const;

		const std::vector<std::string>* sourceLines(const std::string& path);
		void writeOutput(std::span<const u8> bytes);
		void ensureLineStart();
	};
}

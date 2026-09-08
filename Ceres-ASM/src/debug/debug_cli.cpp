#include "debug_cli.h"

#include "expression.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>

namespace ceres::debug
{
	namespace
	{
		std::vector<std::string> tokenize(const std::string& line)
		{
			std::vector<std::string> tokens;
			usize i = 0;
			while (i < line.size())
			{
				while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
					++i;
				if (i >= line.size())
					break;

				const usize start = i;
				while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])))
					++i;
				tokens.emplace_back(line.substr(start, i - start));
			}
			return tokens;
		}

		std::string shortFile(std::string_view path)
		{
			return std::filesystem::path(path).filename().string();
		}
	}

	// --- Output ---------------------------------------------------------------------------------

	void DebugCLI::writeOutput(std::span<const u8> bytes)
	{
		for (u8 byte : bytes)
		{
			std::fputc(static_cast<int>(byte), stdout);
			_outputAtLineStart = (byte == '\n');
		}
		std::fflush(stdout);
	}

	void DebugCLI::ensureLineStart()
	{
		if (!_outputAtLineStart)
		{
			std::cout << '\n';
			_outputAtLineStart = true;
		}
	}

	// --- Run loop -------------------------------------------------------------------------------

	int DebugCLI::run()
	{
		_session.setOutputHandler([this](std::span<const u8> bytes) { writeOutput(bytes); });
		// A logpoint is the debugger talking, not the program, so it is marked as such rather than
		// mixed into the program's own output.
		_session.setLogHandler([this](std::string_view text)
		{
			ensureLineStart();
			std::cout << "  [log] " << text << '\n';
		});

		printBanner();

		StopEvent event = _session.start();
		printStop(event);

		std::string line;
		while (!_quit)
		{
			ensureLineStart();
			std::cout << "(ceres) " << std::flush;

			if (!std::getline(std::cin, line))
			{
				// End of input - a piped script that ran out, or Ctrl+D. Leaving the machine
				// running would strand it, so this is a quit.
				std::cout << '\n';
				break;
			}

			if (!execute(line))
				break;
		}

		_session.terminate();
		return 0;
	}

	void DebugCLI::printBanner() const
	{
		std::cout << "Ceres debugger. 'h' for the command list, 'q' to quit.\n";
	}

	void DebugCLI::printHelp() const
	{
		std::cout <<
			"  Running\n"
			"    c            continue until a breakpoint, a fault or the end\n"
			"    s            step one source line, entering calls\n"
			"    n            step one source line, running calls to completion\n"
			"    si [count]   step one machine instruction (or `count` of them)\n"
			"    fin          run until the current subroutine returns\n"
			"    until <loc>  run until an address, symbol or file:line is reached\n"
			"    run          restart the program from the beginning\n"
			"\n"
			"  Running backwards (the machine is deterministic, so this is real)\n"
			"    rs           step back one source line\n"
			"    rsi          step back one machine instruction\n"
			"    rc           run backwards to the previous breakpoint\n"
			"    goto <tick>  go to a given instruction count\n"
			"    hist         how far back the recording reaches, and what it costs\n"
			"\n"
			"  Breakpoints\n"
			"    b <loc>              break at file:line, a label, or *0x400\n"
			"    b <loc> if <expr>    break only when the expression is true\n"
			"    log <loc> <text>     log {expressions} and carry on instead of stopping\n"
			"    watch [read|rw] <name|addr> [size]   stop when that memory is written,\n"
			"                                         or read, or either\n"
			"    d <id>       delete a breakpoint or watch; `d` alone deletes them all\n"
			"    bl           list breakpoints and watches\n"
			"\n"
			"  Inspecting\n"
			"    regs         registers and flags\n"
			"    bt           call stack\n"
			"    l [loc]      source around the current line\n"
			"    dis [count]  disassembly around the program counter\n"
			"    x <loc> [n]  dump n bytes of memory; `x` alone continues the last dump\n"
			"    vars         global variables and constants\n"
			"    cov          how many times each line has run; zero means never reached\n"
			"    p <expr>     evaluate: r3, sp < 0x1000, total, scores[2], [r1 + 4], u8[r2]\n"
			"\n"
			"  Changing things\n"
			"    set <reg> <value>   write a register (r0-r15, f0-f15, sp, fp, at, pc)\n"
			"    input <text>        feed a line to the program's terminal input\n"
			"\n"
			"    q            quit\n";
	}

	void DebugCLI::printStop(const StopEvent& event)
	{
		ensureLineStart();

		switch (event.reason)
		{
			case StopReason::Exited:
				std::cout << "Program exited.\n";
				_quit = true;
				return;

			case StopReason::Halted:
				std::cout << "The machine is halted with no interrupt source armed; nothing can wake it.\n";
				break;

			case StopReason::StepLimit:
				std::cout << "Still running after " << DebugSession::DefaultStepLimit
					<< " instructions - stopped so you can look. 'c' to carry on.\n";
				break;

			case StopReason::Error:
				std::cout << "Error: " << event.message << '\n';
				return;

			case StopReason::Exception:
				std::cout << event.message << '\n';
				break;

			case StopReason::Breakpoint:
				std::cout << "Breakpoint " << event.breakpoint << ", " << event.message << '\n';
				break;

			case StopReason::DataBreakpoint:
				std::cout << "Watch " << event.dataBreakpoint << ", " << event.message << '\n';
				break;

			default:
				std::cout << event.message << '\n';
				break;
		}

		// Where you are matters more than why you stopped, so it always follows.
		if (const auto location = _session.currentLocation(); location.has_value())
		{
			_listCentre = location->expansionLine;
			printSourceContext(location->expansionLine, 2);
		}
		else
		{
			printDisassembly(_session.programCounter(), 4);
		}
	}

	// --- Source ---------------------------------------------------------------------------------

	const std::vector<std::string>* DebugCLI::sourceLines(const std::string& path)
	{
		if (const auto it = _sourceCache.find(path); it != _sourceCache.end())
			return it->second.empty() ? nullptr : &it->second;

		std::vector<std::string> lines;
		std::ifstream file(path);
		if (file)
		{
			std::string line;
			while (std::getline(file, line))
			{
				// The tutorial's files are CRLF; leaving the carriage return in makes every
				// printed line overwrite its own start.
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				lines.push_back(std::move(line));
			}
		}

		auto [it, _] = _sourceCache.emplace(path, std::move(lines));
		return it->second.empty() ? nullptr : &it->second;
	}

	void DebugCLI::printSourceContext(u32 around, u32 radius)
	{
		const auto location = _session.currentLocation();
		if (!location.has_value())
			return;

		const std::string path{ location->expansionFile };
		const std::vector<std::string>* lines = sourceLines(path);
		if (lines == nullptr)
		{
			std::cout << "  (source for " << shortFile(path) << " is not readable from here)\n";
			return;
		}

		const u32 currentLine = location->expansionLine;
		const u32 first = around > radius ? around - radius : 1;
		const u32 last = std::min<u32>(around + radius, static_cast<u32>(lines->size()));

		for (u32 line = first; line <= last; ++line)
		{
			// The arrow marks the line about to run, which is not always the one being listed.
			std::cout << std::format("{} {:>4} | {}\n",
				line == currentLine ? "->" : "  ", line, (*lines)[line - 1]);
		}
	}

	// --- Views ----------------------------------------------------------------------------------

	void DebugCLI::printRegisters() const
	{
		const RegisterView view = _session.registers();

		for (usize i = 0; i < view.general.size(); i += 4)
		{
			for (usize j = i; j < i + 4 && j < view.general.size(); ++j)
				std::cout << std::format("  r{:<2} ={:#010x}", j, view.general[j]);
			std::cout << '\n';
		}

		std::cout << std::format("  sp  ={:#010x}  fp  ={:#010x}  at  ={:#010x}  pc  ={:#010x}\n",
			view.general[vm::GeneralPurposeRegisterPool::StackPointerIndex],
			view.general[vm::GeneralPurposeRegisterPool::FramePointerIndex],
			view.general[vm::GeneralPurposeRegisterPool::AssemblerTempIndex],
			view.programCounter);

		std::cout << std::format("  flags: Z={} S={} C={} O={} I={} H={} T={}   ticks: {}\n",
			view.zero ? 1 : 0, view.sign ? 1 : 0, view.carry ? 1 : 0, view.overflow ? 1 : 0,
			view.interruptEnabled ? 1 : 0, view.halting ? 1 : 0, view.trap ? 1 : 0,
			view.executedInstructions);

		// The float bank is only printed when something is in it: sixteen zeroes every time would
		// bury the registers that matter.
		const bool anyFloat = std::ranges::any_of(view.floating, [](f32 value) { return value != 0.0f; });
		if (anyFloat)
		{
			for (usize i = 0; i < view.floating.size(); i += 4)
			{
				for (usize j = i; j < i + 4 && j < view.floating.size(); ++j)
					std::cout << std::format("  f{:<2} ={:>12}", j, view.floating[j]);
				std::cout << '\n';
			}
		}
	}

	void DebugCLI::printBacktrace() const
	{
		// Walked through the frame pointers when the assembler recorded that this function opens
		// one; only then is the stack exact rather than inferred from instructions gone past.
		const std::vector<Frame> exact = _session.unwindCallStack();
		const std::span<const Frame> reconstructed = _session.callStack();
		const std::span<const Frame> frames = exact.empty()
			? reconstructed
			: std::span<const Frame>(exact);

		if (frames.empty())
		{
			std::cout << "  (no frames)\n";
			return;
		}

		// Innermost first, the way every debugger prints it. The reconstruction grows outward,
		// so it reads backwards; the walk already starts at the innermost frame.
		for (usize n = 0; n < frames.size(); ++n)
		{
			const usize i = exact.empty() ? frames.size() - 1 - n : n;
			const Frame& frame = frames[i];
			std::string where = std::format("{:#010x}", frame.address);
			if (frame.location.has_value())
			{
				where = std::format("{}:{}",
					shortFile(frame.location->expansionFile), frame.location->expansionLine);
			}

			std::cout << std::format("  #{} {:<28} {}{}\n",
				n,
				frame.name,
				where,
				frame.isInterruptHandler ? "  (interrupt)" : "");
		}

		if (exact.empty())
			std::cout << "  (reconstructed: CALL only pushes a return address, so frames are inferred)\n";
		else
			std::cout << "  (walked through the frame pointers, so these are exact)\n";
	}

	void DebugCLI::printBreakpoints() const
	{
		const auto breakpoints = _session.breakpoints();
		const auto watches = _session.dataBreakpoints();
		if (breakpoints.empty() && watches.empty())
		{
			std::cout << "  (none)\n";
			return;
		}

		for (const Breakpoint& breakpoint : breakpoints)
		{
			std::string where;
			switch (breakpoint.kind)
			{
				case BreakpointKind::Line:
					where = std::format("{}:{}", shortFile(breakpoint.file), breakpoint.line);
					break;
				case BreakpointKind::Symbol:
					where = breakpoint.symbol;
					break;
				case BreakpointKind::Address:
					where = std::format("*{:#010x}", breakpoint.address);
					break;
			}

			std::cout << std::format("  {:>2}  {:<24} {:#010x}  hits: {}\n",
				breakpoint.id, where, breakpoint.address, breakpoint.hitCount);

			if (!breakpoint.options.condition.empty())
				std::cout << std::format("        if {}\n", breakpoint.options.condition);
			if (!breakpoint.options.hitCondition.empty())
				std::cout << std::format("        hit count {}\n", breakpoint.options.hitCondition);
			if (!breakpoint.options.logMessage.empty())
				std::cout << std::format("        log \"{}\"\n", breakpoint.options.logMessage);
		}

		for (const DataBreakpoint& watch : watches)
		{
			std::cout << std::format("  {:>2}  watch {:<18} {:#010x} +{}  hits: {}\n",
				watch.id, watch.label, watch.address, watch.size, watch.hitCount);
		}
	}

	void DebugCLI::printDisassembly(u32 around, u32 count)
	{
		const u32 pc = _session.programCounter();
		const auto lines = _session.disassemble(around, count / 2, count);

		for (const DisassembledInstruction& line : lines)
		{
			if (!line.symbol.empty())
				std::cout << line.symbol << ":\n";

			std::string source;
			if (line.location.has_value())
			{
				source = std::format("  ; {}:{}{}",
					shortFile(line.location->expansionFile),
					line.location->expansionLine,
					line.location->isPadding() ? " (padding)" : "");
			}

			std::cout << std::format("{} {:08x}  {:08x}  {:<26}{}\n",
				line.address == pc ? "->" : "  ", line.address, line.raw, line.text, source);
		}
	}

	void DebugCLI::printGlobals() const
	{
		const auto globals = _session.globals();
		if (globals.empty())
		{
			std::cout << "  (no variables or constants)\n";
			return;
		}

		for (const VariableView& variable : globals)
		{
			if (variable.isConstant)
				std::cout << std::format("  {:<20} {:<10} {:<12} = {}\n", variable.name, "const", variable.type, variable.value);
			else
				std::cout << std::format("  {:<20} {:#010x} {:<12} = {}\n", variable.name, variable.address, variable.type, variable.value);
		}
	}

	void DebugCLI::dumpMemory(u32 address, u32 count)
	{
		const std::vector<u8> bytes = _session.readMemory(address, count);
		if (bytes.empty())
		{
			std::cout << "  (nothing readable at " << std::format("{:#010x}", address) << ")\n";
			return;
		}

		for (usize offset = 0; offset < bytes.size(); offset += 16)
		{
			std::cout << std::format("  {:08x} ", address + static_cast<u32>(offset));

			const usize end = std::min(offset + 16, bytes.size());
			for (usize i = offset; i < offset + 16; ++i)
				std::cout << (i < end ? std::format(" {:02x}", bytes[i]) : "   ");

			std::cout << "  ";
			for (usize i = offset; i < end; ++i)
			{
				const u8 byte = bytes[i];
				std::cout << static_cast<char>(byte >= 0x20 && byte < 0x7F ? byte : '.');
			}
			std::cout << '\n';
		}

		_nextDumpAddress = address + static_cast<u32>(bytes.size());
	}

	// --- Address parsing ------------------------------------------------------------------------

	std::optional<u32> DebugCLI::parseNumber(std::string_view text) const
	{
		if (text.empty())
			return std::nullopt;

		int base = 10;
		if (text.starts_with("0x") || text.starts_with("0X"))
		{
			base = 16;
			text.remove_prefix(2);
		}

		u32 value = 0;
		const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
		if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
			return std::nullopt;
		return value;
	}

	std::optional<u32> DebugCLI::resolveAddress(std::string_view text) const
	{
		if (text.empty())
			return std::nullopt;

		// *0x400 - an address, spelled the way gdb spells it.
		if (text.front() == '*')
			return parseNumber(text.substr(1));

		// file:line, but only when the part after the colon is a number: a Windows path starts
		// with a drive letter and a colon of its own.
		if (const usize colon = text.rfind(':'); colon != std::string_view::npos && colon + 1 < text.size())
		{
			if (const auto line = parseNumber(text.substr(colon + 1)); line.has_value())
			{
				if (const auto address = _session.debugInfo().firstAddressOfLine(text.substr(0, colon), line.value()))
					return address;
			}
		}

		// A bare number is a line in the current file, which is what a `l 40` most likely means,
		// unless it is out of range for one - then it is an address.
		if (const auto number = parseNumber(text); number.has_value())
		{
			if (const auto here = _session.currentLocation(); here.has_value())
			{
				if (const auto address = _session.debugInfo().firstAddressOfLine(here->expansionFile, number.value()))
					return address;
			}
			return number;
		}

		if (const SymbolEntry* symbol = _session.debugInfo().symbolNamed(text); symbol != nullptr)
			return symbol->address;

		return std::nullopt;
	}

	// --- Commands -------------------------------------------------------------------------------

	bool DebugCLI::execute(const std::string& line)
	{
		const std::vector<std::string> tokens = tokenize(line);
		if (tokens.empty())
			return true;

		const std::string& command = tokens[0];
		const auto argument = [&](usize index) -> std::string_view
		{
			return index < tokens.size() ? std::string_view(tokens[index]) : std::string_view{};
		};

		if (command == "q" || command == "quit")
			return false;

		if (command == "h" || command == "help" || command == "?")
		{
			printHelp();
			return true;
		}

		if (command == "c" || command == "cont" || command == "continue")
		{
			printStop(_session.resume());
			return !_quit;
		}

		if (command == "s" || command == "step")
		{
			printStop(_session.stepLine());
			return !_quit;
		}

		if (command == "n" || command == "next")
		{
			printStop(_session.stepOver());
			return !_quit;
		}

		if (command == "si")
		{
			const u32 count = parseNumber(argument(1)).value_or(1);
			StopEvent event{};
			for (u32 i = 0; i < std::max<u32>(count, 1); ++i)
			{
				event = _session.stepInstruction();
				if (event.reason != StopReason::Step)
					break;
			}
			printStop(event);
			return !_quit;
		}

		if (command == "fin" || command == "finish")
		{
			printStop(_session.stepOut());
			return !_quit;
		}

		if (command == "until")
		{
			const auto address = resolveAddress(argument(1));
			if (!address.has_value())
			{
				std::cout << "  Cannot work out where '" << argument(1) << "' is.\n";
				return true;
			}
			printStop(_session.runToAddress(address.value()));
			return !_quit;
		}

		if (command == "rsi")
		{
			printStop(_session.stepBackInstruction());
			return !_quit;
		}

		if (command == "rs")
		{
			printStop(_session.stepBackLine());
			return !_quit;
		}

		if (command == "rc")
		{
			printStop(_session.reverseContinue());
			return !_quit;
		}

		if (command == "goto")
		{
			const auto tick = parseNumber(argument(1));
			if (!tick.has_value())
			{
				std::cout << "  Usage: goto <instruction count>\n";
				return true;
			}
			printStop(_session.runToTick(tick.value()));
			return !_quit;
		}

		if (command == "hist")
		{
			const History& history = _session.history();
			if (!history.isEnabled())
			{
				std::cout << "  Not recording, so this session cannot go backwards.\n";
				return true;
			}

			std::cout << std::format(
				"  {} snapshots, every {} instructions, reaching back to instruction {} (now at {}).\n"
				"  Memory: {} KiB.\n",
				history.snapshotCount(), history.settings().interval, history.oldestTick(),
				_session.currentTick(), history.memoryCost() / 1024);
			return true;
		}

		if (command == "cov" || command == "coverage")
		{
			const auto entries = _session.coverage();
			if (entries.empty())
			{
				std::cout << "  (nothing to report)\n";
				return true;
			}

			// Grouped by source line: an address count is a machine fact, a line count is what a
			// programmer is looking for. Only the head of each line carries the total.
			u64 total = 0;
			usize unreached = 0;
			for (const CoverageEntry& entry : entries)
			{
				total += entry.count;
				if (entry.count == 0)
					++unreached;

				if (!entry.location.has_value() ||
					(entry.location->flags & LineFlag::FirstOfLine) == 0)
				{
					continue;
				}

				std::cout << std::format("  {:>10}  {}:{}\n",
					entry.count == 0 ? std::string("never") : std::format("{}", entry.count),
					shortFile(entry.location->expansionFile),
					entry.location->expansionLine);
			}

			std::cout << std::format("  -- {} instructions retired, {} of {} words never reached\n",
				total, unreached, entries.size());
			return true;
		}

		if (command == "run" || command == "r")
		{
			_quit = false;
			printStop(_session.restart());
			return !_quit;
		}

		if (command == "b" || command == "break")
		{
			const std::string_view target = argument(1);
			if (target.empty())
			{
				std::cout << "  Usage: b file:line | b label | b *0x400 [if <expression>]\n";
				return true;
			}

			// Everything after a bare `if` is the condition, taken verbatim so it can contain
			// spaces: `b 42 if r3 == 10`.
			BreakpointOptions options;
			for (usize i = 2; i < tokens.size(); ++i)
			{
				if (tokens[i] != "if")
					continue;

				const usize conditionStart = line.find(" if ", line.find(target));
				if (conditionStart != std::string::npos)
					options.condition = line.substr(conditionStart + 4);
				break;
			}

			std::expected<BreakpointId, std::string> added = std::unexpected("");
			if (target.front() == '*')
			{
				const auto address = parseNumber(target.substr(1));
				added = address.has_value()
					? _session.addAddressBreakpoint(address.value(), options)
					: std::unexpected(std::format("'{}' is not an address", target));
			}
			else if (const usize colon = target.rfind(':');
				colon != std::string_view::npos && parseNumber(target.substr(colon + 1)).has_value())
			{
				added = _session.addLineBreakpoint(
					target.substr(0, colon), parseNumber(target.substr(colon + 1)).value(), options);
			}
			else if (const auto lineOnly = parseNumber(target); lineOnly.has_value())
			{
				// A bare number means a line in the file the machine is stopped in, which is
				// almost always the file being read on screen.
				const auto here = _session.currentLocation();
				added = here.has_value()
					? _session.addLineBreakpoint(here->expansionFile, lineOnly.value(), options)
					: std::unexpected("There is no current file to take a line number from");
			}
			else
			{
				added = _session.addSymbolBreakpoint(target, options);
			}

			if (added.has_value())
			{
				const Breakpoint& breakpoint = _session.breakpoints().back();
				std::cout << std::format("  Breakpoint {} at {:#010x}", added.value(), breakpoint.address);
				if (!breakpoint.file.empty())
					std::cout << std::format(" ({}:{})", shortFile(breakpoint.file), breakpoint.line);
				std::cout << '\n';
			}
			else
			{
				std::cout << "  " << added.error() << '\n';
			}
			return true;
		}

		if (command == "log")
		{
			// A logpoint is a breakpoint that reports and carries on, so it is set the same way
			// with the message taking everything after the location.
			const std::string_view target = argument(1);
			if (target.empty() || tokens.size() < 3)
			{
				std::cout << "  Usage: log <loc> <message>, where {expressions} are interpolated\n";
				return true;
			}

			const usize messageStart = line.find(target) + target.size();
			BreakpointOptions options;
			options.logMessage = line.substr(messageStart);
			while (!options.logMessage.empty() && options.logMessage.front() == ' ')
				options.logMessage.erase(0, 1);

			std::expected<BreakpointId, std::string> added = std::unexpected("");
			if (const auto lineOnly = parseNumber(target); lineOnly.has_value())
			{
				const auto here = _session.currentLocation();
				added = here.has_value()
					? _session.addLineBreakpoint(here->expansionFile, lineOnly.value(), std::move(options))
					: std::unexpected("There is no current file to take a line number from");
			}
			else if (const usize colon = target.rfind(':');
				colon != std::string_view::npos && parseNumber(target.substr(colon + 1)).has_value())
			{
				added = _session.addLineBreakpoint(
					target.substr(0, colon), parseNumber(target.substr(colon + 1)).value(), std::move(options));
			}
			else
			{
				added = _session.addSymbolBreakpoint(target, std::move(options));
			}

			if (added.has_value())
				std::cout << std::format("  Logpoint {} at {}\n", added.value(), target);
			else
				std::cout << "  " << added.error() << '\n';
			return true;
		}

		if (command == "watch")
		{
			// `watch read x` and `watch rw x` ask for the accesses a write-only watch cannot see.
			usize firstArgument = 1;
			debug::WatchMode mode = debug::WatchMode::Write;
			if (argument(1) == "read" || argument(1) == "r")
			{
				mode = debug::WatchMode::Read;
				firstArgument = 2;
			}
			else if (argument(1) == "rw")
			{
				mode = debug::WatchMode::ReadWrite;
				firstArgument = 2;
			}

			const std::string_view target = argument(firstArgument);
			if (target.empty())
			{
				std::cout << "  Usage: watch [read|rw] <variable> | watch [read|rw] 0x1000 [size]\n";
				return true;
			}

			u32 address = 0;
			u32 size = 4;

			// A named variable knows its own extent, which is almost always what you meant.
			if (const SymbolEntry* symbol = _session.debugInfo().symbolNamed(target);
				symbol != nullptr && symbol->size > 0)
			{
				address = symbol->address;
				size = symbol->size;
			}
			else if (const auto resolved = resolveAddress(target); resolved.has_value())
			{
				address = resolved.value();
			}
			else
			{
				std::cout << "  Cannot work out where '" << target << "' is.\n";
				return true;
			}

			if (!argument(firstArgument + 1).empty())
				size = parseNumber(argument(firstArgument + 1)).value_or(size);

			auto added = _session.addDataBreakpoint(address, size, std::string(target), mode);
			if (added.has_value())
				std::cout << std::format("  Watch {} on {} bytes at {:#010x}\n", added.value(), size, address);
			else
				std::cout << "  " << added.error() << '\n';
			return true;
		}

		if (command == "d" || command == "delete")
		{
			if (argument(1).empty())
			{
				_session.clearBreakpoints();
				_session.clearDataBreakpoints();
				std::cout << "  All breakpoints and watches deleted.\n";
				return true;
			}

			const auto id = parseNumber(argument(1));
			// Breakpoints and watches share one numbering, so one command deletes either.
			const bool removed = id.has_value() &&
				(_session.removeBreakpoint(id.value()) || _session.removeDataBreakpoint(id.value()));
			if (!removed)
				std::cout << "  No breakpoint or watch " << argument(1) << ".\n";
			return true;
		}

		if (command == "bl" || command == "breaks")
		{
			printBreakpoints();
			return true;
		}

		if (command == "regs" || command == "reg" || command == "i")
		{
			printRegisters();
			return true;
		}

		if (command == "bt" || command == "where")
		{
			printBacktrace();
			return true;
		}

		if (command == "l" || command == "list")
		{
			if (!argument(1).empty())
			{
				if (const auto line = parseNumber(argument(1)); line.has_value())
					_listCentre = line.value();
			}
			printSourceContext(_listCentre, 5);
			return true;
		}

		if (command == "dis" || command == "disas")
		{
			const u32 count = parseNumber(argument(1)).value_or(10);
			printDisassembly(_session.programCounter(), count);
			return true;
		}

		if (command == "x")
		{
			u32 address = _nextDumpAddress;
			u32 count = 64;

			if (!argument(1).empty())
			{
				const auto resolved = resolveAddress(argument(1));
				if (!resolved.has_value())
				{
					std::cout << "  Cannot work out where '" << argument(1) << "' is.\n";
					return true;
				}
				address = resolved.value();

				// A named variable knows its own size, so `x msg` dumps exactly the variable.
				if (const SymbolEntry* symbol = _session.debugInfo().symbolNamed(argument(1));
					symbol != nullptr && symbol->size > 0)
				{
					count = symbol->size;
				}
			}

			if (!argument(2).empty())
				count = parseNumber(argument(2)).value_or(count);

			dumpMemory(address, count);
			return true;
		}

		if (command == "vars")
		{
			printGlobals();
			return true;
		}

		if (command == "p" || command == "print")
		{
			// Everything after the command word, so an expression can contain spaces.
			const usize expressionStart = line.find(command) + command.size();
			std::string expression = line.substr(expressionStart);
			while (!expression.empty() && expression.front() == ' ')
				expression.erase(0, 1);

			if (expression.empty())
			{
				std::cout << "  Usage: p <expression>\n";
				return true;
			}

			auto value = _session.evaluate(expression);
			if (!value.has_value())
			{
				std::cout << "  " << value.error() << '\n';
				return true;
			}

			std::cout << std::format("  {} : {} = {}", expression, value->type, value->text);
			if (value->address.has_value())
				std::cout << std::format("   at {:#010x}", value->address.value());
			std::cout << '\n';
			return true;
		}

		if (command == "set")
		{
			const auto value = parseNumber(argument(2));
			if (argument(1).empty() || !value.has_value())
			{
				std::cout << "  Usage: set <register> <value>\n";
				return true;
			}
			if (_session.setRegister(argument(1), value.value()))
				std::cout << std::format("  {} = {:#010x}\n", argument(1), value.value());
			else
				std::cout << "  '" << argument(1) << "' is not a register this machine has.\n";
			return true;
		}

		if (command == "input")
		{
			// Everything after the command word, verbatim, plus the newline a line-based program
			// is waiting for.
			const usize commandEnd = line.find(command) + command.size();
			std::string text = line.substr(commandEnd);
			if (!text.empty() && text.front() == ' ')
				text.erase(0, 1);
			text += '\n';
			_session.pushInput(text);
			std::cout << std::format("  Queued {} byte(s) of input.\n", text.size());
			return true;
		}

		std::cout << "  Unknown command '" << command << "'. 'h' for the list.\n";
		return true;
	}
}

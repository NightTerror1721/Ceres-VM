#include "debug_session.h"

#include "assembler/assembler.h"
#include "vm/disassembler.h"
#include "vm/opcodes.h"
#include <algorithm>
#include <bit>
#include <format>

namespace ceres::debug
{
	namespace
	{
		constexpr u32 InstructionSize = vm::Instruction::Size;

		bool isCall(vm::Opcode opcode) noexcept
		{
			return opcode == vm::Opcode::CALL || opcode == vm::Opcode::CALLR;
		}

		bool isAnyReturn(vm::Opcode opcode) noexcept
		{
			return opcode == vm::Opcode::RET || opcode == vm::Opcode::IRET;
		}

		usize scalarSize(ScalarType type) noexcept
		{
			switch (type)
			{
				case ScalarType::U8:
				case ScalarType::I8:  return 1;
				case ScalarType::U16:
				case ScalarType::I16: return 2;
				case ScalarType::U32:
				case ScalarType::I32:
				case ScalarType::F32: return 4;
				default:              return 0;
			}
		}

		std::string_view scalarName(ScalarType type) noexcept
		{
			switch (type)
			{
				case ScalarType::U8:  return "u8";
				case ScalarType::U16: return "u16";
				case ScalarType::U32: return "u32";
				case ScalarType::I8:  return "i8";
				case ScalarType::I16: return "i16";
				case ScalarType::I32: return "i32";
				case ScalarType::F32: return "f32";
				default:              return "?";
			}
		}

		std::string renderScalar(ScalarType type, std::span<const u8> bytes)
		{
			u32 value = 0;
			const usize size = std::min(scalarSize(type), bytes.size());
			for (usize i = 0; i < size; ++i)
				value |= static_cast<u32>(bytes[i]) << (8 * i);

			switch (type)
			{
				case ScalarType::U8:  return std::format("{}", static_cast<u8>(value));
				case ScalarType::U16: return std::format("{}", static_cast<u16>(value));
				case ScalarType::U32: return std::format("{}", value);
				case ScalarType::I8:  return std::format("{}", static_cast<i8>(value));
				case ScalarType::I16: return std::format("{}", static_cast<i16>(value));
				case ScalarType::I32: return std::format("{}", static_cast<i32>(value));
				case ScalarType::F32: return std::format("{}", std::bit_cast<f32>(value));
				default:              return "?";
			}
		}
	}

	std::string_view describe(StopReason reason) noexcept
	{
		switch (reason)
		{
			case StopReason::Entry:      return "entry";
			case StopReason::Breakpoint: return "breakpoint";
			case StopReason::Step:       return "step";
			case StopReason::Pause:      return "pause";
			case StopReason::Halted:     return "halted";
			case StopReason::Exception:  return "exception";
			case StopReason::Exited:     return "exited";
			case StopReason::StepLimit:  return "step limit";
			case StopReason::Error:      return "error";
		}
		return "unknown";
	}

	std::string_view describe(vm::InterruptNumber number) noexcept
	{
		switch (number)
		{
			case vm::InterruptNumber::Reset:              return "Reset";
			case vm::InterruptNumber::Trap:               return "Trap";
			case vm::InterruptNumber::IllegalInstruction: return "IllegalInstruction";
			case vm::InterruptNumber::MemoryFault:        return "MemoryFault";
			case vm::InterruptNumber::DivisionByZero:     return "DivisionByZero";
			case vm::InterruptNumber::StackOverflow:      return "StackOverflow";
			case vm::InterruptNumber::AlignmentFault:     return "AlignmentFault";
			case vm::InterruptNumber::Syscall:            return "Syscall";
			default:                                      return "UserInterrupt";
		}
	}

	// --- Construction ---------------------------------------------------------------------------

	DebugSession::DebugSession(vm::Program&& program, DebugInfo&& debugInfo, const LaunchConfig& config) :
		_program(std::move(program)),
		_debugInfo(std::move(debugInfo)),
		_config(config)
	{}

	std::expected<std::unique_ptr<DebugSession>, std::string> DebugSession::launch(const LaunchConfig& config)
	{
		if (config.sources.empty())
			return std::unexpected("No program to debug");

		for (const auto& source : config.sources)
		{
			if (!std::filesystem::exists(source))
				return std::unexpected("No such file: " + source.string());
		}

		std::optional<vm::Program> program;
		DebugInfo debugInfo;

		if (config.sources.size() == 1 && config.sources.front().extension() == ".cres")
		{
			auto loaded = vm::Program::loadFromFile(config.sources.front());
			if (!loaded)
				return std::unexpected(loaded.error());

			if (loaded->hasDebugSection())
			{
				// A .cres built without --debug still debugs, just blind: addresses and registers
				// but no source lines. Better than refusing to start.
				if (auto parsed = DebugInfo::deserialize(loaded->debugSection()))
					debugInfo = std::move(parsed.value());
			}
			program = std::move(loaded.value());
		}
		else
		{
			// Always with debug information: a debug session is exactly the case that wants it,
			// and assembling in memory means it never has to be written anywhere.
			casm::Assembler assembler{ casm::AssemblerOptions{ .emitDebugInfo = true } };
			auto assembled = assembler.assemble(config.sources);
			if (!assembled.has_value() || assembler.hasErrors())
			{
				std::string message = "Failed to assemble";
				for (const auto& error : assembler.errors())
					message += std::format("\n  [{}:{}] {}", error.file, error.line, error.message);
				return std::unexpected(message);
			}
			program = std::move(assembled.value());
			debugInfo = assembler.debugInfo();
		}

		// `new` rather than make_unique because the constructor is private: launch() is the only
		// way to get one of these, and it has to stay that way for the machine to be set up.
		std::unique_ptr<DebugSession> session{
			new DebugSession(std::move(program.value()), std::move(debugInfo), config) };

		session->_vm = std::make_unique<vm::CeresVM>(config.memorySize);
		session->attachDevices();

		if (auto loaded = session->_vm->loadProgram(session->_program); !loaded)
			return std::unexpected(loaded.error());

		return session;
	}

	void DebugSession::attachDevices()
	{
		// Both control commands end the session: a program asking for a reset while under a
		// debugger is telling you something, and silently rebooting would hide it.
		_systemControl = std::make_unique<vm::SystemControlDevice>(
			[this]() { _vm->shutdown(); },
			[this]() { _vm->shutdown(); });
		_systemControl->attachTo(_vm->io());

		_terminal = std::make_unique<vm::TerminalDevice>();
		_terminal->attachTo(_vm->io());

		_timer = std::make_unique<vm::TimerDevice>();
		_timer->attachTo(_vm->io());

		// Recorded rather than acted on: triggerInterrupt is noexcept and in the middle of
		// redirecting the machine, so all this does is leave a note for stepOnce to read once the
		// instruction is over.
		_vm->engine().setInterruptObserver(
			[this](vm::InterruptNumber number, vm::Address atPc, bool entered) noexcept
			{
				_lastInterrupt = PendingInterrupt{ number, atPc.value(), entered, true };
			});
	}

	void DebugSession::setOutputHandler(OutputHandler handler)
	{
		_outputHandler = std::move(handler);
		if (!_terminal)
			return;

		if (_outputHandler)
		{
			_terminal->setOutputSink([this](u8 byte)
			{
				// One byte at a time, undecoded: a multi-byte UTF-8 character is several separate
				// port writes, so only the consumer can know where a character ends.
				if (_outputHandler)
					_outputHandler(std::span<const u8>(&byte, 1));
			});
		}
		else
		{
			_terminal->clearOutputSink();
		}
	}

	void DebugSession::pushInput(std::string_view text)
	{
		if (_terminal)
			_terminal->pushInput(text);
	}

	// --- Lifecycle ------------------------------------------------------------------------------

	StopEvent DebugSession::start()
	{
		if (_started)
		{
			StopEvent event = makeStop(StopReason::Error);
			event.message = "The session has already been started";
			return event;
		}

		if (auto powered = _vm->powerOn(); !powered)
		{
			StopEvent event = makeStop(StopReason::Error);
			event.message = powered.error();
			return event;
		}

		_started = true;
		_terminated = false;
		resetCallStack();

		if (_config.stopOnEntry)
			return makeStop(StopReason::Entry);

		return resume();
	}

	void DebugSession::terminate()
	{
		if (_vm)
		{
			_vm->shutdown();
			_vm->engine().clearInterruptObserver();
		}
		_terminated = true;
	}

	StopEvent DebugSession::restart()
	{
		// A fresh machine rather than a reset one: .data has been written to by now, and a reset
		// only rewinds the registers.
		OutputHandler handler = std::move(_outputHandler);
		_vm = std::make_unique<vm::CeresVM>(_config.memorySize);
		attachDevices();
		setOutputHandler(std::move(handler));

		if (auto loaded = _vm->loadProgram(_program); !loaded)
		{
			StopEvent event = makeStop(StopReason::Error);
			event.message = loaded.error();
			return event;
		}

		_started = false;
		_terminated = false;
		_lastInterrupt = {};
		for (Breakpoint& breakpoint : _breakpoints)
			breakpoint.hitCount = 0;

		return start();
	}

	// --- Stepping -------------------------------------------------------------------------------

	u32 DebugSession::programCounter() const
	{
		return _vm->engine().programCounter().value();
	}

	StopEvent DebugSession::makeStop(StopReason reason) const
	{
		StopEvent event;
		event.reason = reason;
		event.address = programCounter();

		if (const auto location = currentLocation(); location.has_value())
		{
			event.message = std::format("{} at {}:{} ({:#010x})",
				describe(reason),
				std::filesystem::path(location->expansionFile).filename().string(),
				location->expansionLine,
				event.address);
		}
		else
		{
			event.message = std::format("{} at {:#010x}", describe(reason), event.address);
		}

		return event;
	}

	Breakpoint* DebugSession::breakpointAt(u32 address)
	{
		for (Breakpoint& breakpoint : _breakpoints)
		{
			if (breakpoint.verified && breakpoint.address == address)
				return &breakpoint;
		}
		return nullptr;
	}

	StopEvent DebugSession::stepOnce()
	{
		if (_terminated || !_started)
		{
			StopEvent event = makeStop(StopReason::Error);
			event.message = "The session is not running";
			return event;
		}

		if (!_vm->isPoweredOn())
			return makeStop(StopReason::Exited);

		vm::ExecutionEngine& engine = _vm->engine();

		const u32 pcBefore = engine.programCounter().value();
		const bool wasHalted = engine.isHalted();
		const vm::Instruction executed = _vm->memory().readInstruction(vm::Address(pcBefore));

		_lastInterrupt = {};
		engine.step();

		const u32 pcAfter = engine.programCounter().value();
		const u32 spAfter = engine.registers().sp().value();

		// A halted machine executes nothing, so the only thing that can move the stack is an
		// interrupt arriving to wake it.
		if (!wasHalted || (_lastInterrupt.valid && _lastInterrupt.entered))
			updateCallStack(executed, pcBefore, pcAfter, spAfter, wasHalted);

		refreshTopFrame();

		if (!_vm->isPoweredOn())
			return makeStop(StopReason::Exited);

		// A fault reports where it happened, not where it went: by now the program counter is in
		// the handler, and the faulting instruction is the thing the user needs to see.
		if (_lastInterrupt.valid &&
			static_cast<u8>(_lastInterrupt.number) < vm::ReservedInterruptCount &&
			_lastInterrupt.number != vm::InterruptNumber::Reset)
		{
			StopEvent event = makeStop(StopReason::Exception);
			event.exception = _lastInterrupt.number;
			event.exceptionAddress = _lastInterrupt.atAddress;

			std::string where = std::format("{:#010x}", _lastInterrupt.atAddress);
			if (const auto location = _debugInfo.locationOf(_lastInterrupt.atAddress); location.has_value())
			{
				where = std::format("{}:{}",
					std::filesystem::path(location->expansionFile).filename().string(),
					location->expansionLine);
			}

			event.message = _lastInterrupt.entered
				? std::format("{} raised at {}", describe(_lastInterrupt.number), where)
				: std::format("{} raised at {}, with no handler installed", describe(_lastInterrupt.number), where);
			return event;
		}

		return makeStop(StopReason::Step);
	}

	StopEvent DebugSession::runUntil(const std::function<bool()>& shouldStop, u64 maxInstructions)
	{
		if (!_started || _terminated)
		{
			StopEvent event = makeStop(StopReason::Error);
			event.message = "The session is not running";
			return event;
		}

		_pauseRequested.store(false, std::memory_order_release);

		for (u64 executed = 0; executed < maxInstructions; ++executed)
		{
			// Checked before the instruction rather than after, so a machine sitting in HALT with
			// nothing left to wake it is reported instead of spinning out the whole budget one
			// millisecond at a time.
			if (_vm->engine().isHalted() && !_vm->interrupts().hasPending() && !_timer->isArmed())
				return makeStop(StopReason::Halted);

			StopEvent event = stepOnce();
			if (event.reason != StopReason::Step)
				return event;

			if (_pauseRequested.exchange(false, std::memory_order_acq_rel))
				return makeStop(StopReason::Pause);

			if (Breakpoint* breakpoint = breakpointAt(programCounter()); breakpoint != nullptr)
			{
				breakpoint->hitCount++;
				StopEvent hit = makeStop(StopReason::Breakpoint);
				hit.breakpoint = breakpoint->id;
				return hit;
			}

			if (shouldStop && shouldStop())
				return makeStop(StopReason::Step);
		}

		return makeStop(StopReason::StepLimit);
	}

	StopEvent DebugSession::stepInstruction()
	{
		StopEvent event = stepOnce();
		if (event.reason != StopReason::Step)
			return event;

		// A breakpoint on the instruction just landed on still counts, so a step and a continue
		// agree about where the machine is allowed to come to rest.
		if (Breakpoint* breakpoint = breakpointAt(programCounter()); breakpoint != nullptr)
		{
			breakpoint->hitCount++;
			event.reason = StopReason::Breakpoint;
			event.breakpoint = breakpoint->id;
			event.message = makeStop(StopReason::Breakpoint).message;
		}
		return event;
	}

	StopEvent DebugSession::stepLine()
	{
		const auto start = currentLocation();
		const usize startDepth = _callStack.size();

		// No line information: the most useful thing "step" can mean is one instruction.
		if (!start.has_value())
			return stepInstruction();

		const u32 startLine = start->expansionLine;
		const std::string startFile{ start->expansionFile };

		const auto reachedNewLine = [this, startLine, startFile, startDepth]()
		{
			// Returning out of a frame always ends the step, or stepping the last line of a
			// subroutine would run straight on into its caller.
			if (_callStack.size() < startDepth)
				return true;

			const auto now = currentLocation();
			if (!now.has_value())
				return false;

			// Only at the head of a line: a `la` is two words and a macro call is many, and
			// stopping inside one of those runs would show the same line several times over.
			if ((now->flags & LineFlag::FirstOfLine) == 0)
				return false;

			return now->expansionLine != startLine || now->expansionFile != startFile;
		};

		return runUntil(reachedNewLine, StepLineLimit);
	}

	StopEvent DebugSession::stepOver()
	{
		const auto start = currentLocation();
		const usize startDepth = _callStack.size();
		const bool hadLocation = start.has_value();
		const u32 startLine = hadLocation ? start->expansionLine : 0;
		const std::string startFile = hadLocation ? std::string(start->expansionFile) : std::string{};

		const auto reachedNewLineAtOrAboveDepth =
			[this, hadLocation, startLine, startFile, startDepth]()
		{
			if (_callStack.size() < startDepth)
				return true;
			// Inside a call made from this line: keep going until it returns.
			if (_callStack.size() > startDepth)
				return false;

			const auto now = currentLocation();
			if (!now.has_value())
				return !hadLocation;
			if ((now->flags & LineFlag::FirstOfLine) == 0)
				return false;
			if (!hadLocation)
				return true;

			return now->expansionLine != startLine || now->expansionFile != startFile;
		};

		return runUntil(reachedNewLineAtOrAboveDepth, StepLineLimit);
	}

	StopEvent DebugSession::stepOut()
	{
		const usize startDepth = _callStack.size();
		if (startDepth <= 1)
		{
			// Nothing to return to; running to the end is the honest reading of "finish".
			return resume();
		}

		return runUntil([this, startDepth]() { return _callStack.size() < startDepth; }, DefaultStepLimit);
	}

	StopEvent DebugSession::runToAddress(u32 address)
	{
		return runUntil([this, address]() { return programCounter() == address; }, DefaultStepLimit);
	}

	StopEvent DebugSession::resume(u64 maxInstructions)
	{
		return runUntil({}, maxInstructions);
	}

	// --- Shadow call stack ----------------------------------------------------------------------

	void DebugSession::resetCallStack()
	{
		_callStack.clear();

		Frame frame;
		frame.address = programCounter();
		frame.entryAddress = frame.address;
		frame.returnAddress = 0;
		frame.stackPointer = _vm->engine().registers().sp().value();
		frame.name = symbolNameAt(frame.address);
		frame.location = _debugInfo.locationOf(frame.address);
		_callStack.push_back(std::move(frame));
	}

	void DebugSession::refreshTopFrame()
	{
		if (_callStack.empty())
			return;

		Frame& top = _callStack.back();
		top.address = programCounter();
		top.location = _debugInfo.locationOf(top.address);
	}

	void DebugSession::updateCallStack(vm::Instruction executed, u32 pcBefore, u32 pcAfter, u32 spAfter, bool wasHalted)
	{
		// An interrupt taken during this instruction outranks whatever the instruction itself was
		// doing: the machine is now in a handler, and that is the frame to show.
		if (_lastInterrupt.valid && _lastInterrupt.entered)
		{
			Frame frame;
			frame.address = pcAfter;
			frame.entryAddress = pcAfter;
			frame.returnAddress = _lastInterrupt.atAddress;
			frame.stackPointer = spAfter;
			frame.isInterruptHandler = true;
			frame.name = std::format("{} handler", describe(_lastInterrupt.number));
			frame.location = _debugInfo.locationOf(pcAfter);
			_callStack.push_back(std::move(frame));
			return;
		}

		if (wasHalted)
			return; // Nothing ran, so nothing can have changed the frames.

		const vm::Opcode opcode = executed.opcode();

		if (isCall(opcode))
		{
			// A CALL whose push overflowed the stack never jumped, so there is no frame to add.
			if (pcAfter == pcBefore + InstructionSize)
				return;

			Frame frame;
			frame.address = pcAfter;
			frame.entryAddress = pcAfter;
			frame.returnAddress = pcBefore + InstructionSize;
			frame.stackPointer = spAfter;
			frame.name = symbolNameAt(pcAfter);
			frame.location = _debugInfo.locationOf(pcAfter);
			_callStack.push_back(std::move(frame));
			return;
		}

		if (isAnyReturn(opcode))
		{
			// The outermost frame is never popped: a RET without its CALL means the program is
			// doing something the shadow stack cannot follow, and an empty stack would be worse
			// than a stale one.
			if (_callStack.size() > 1)
				_callStack.pop_back();
			return;
		}

		// Code that unwinds by hand — popping a return address into a register and jumping, or
		// discarding a frame with an add to the stack pointer — would otherwise leave the shadow
		// stack claiming frames that are gone. Anything that moves the stack pointer above a
		// frame's own entry value has left that frame, whatever instruction did it.
		while (_callStack.size() > 1 && spAfter > _callStack.back().stackPointer)
			_callStack.pop_back();
	}

	// --- Breakpoints ----------------------------------------------------------------------------

	std::expected<BreakpointId, std::string> DebugSession::addAddressBreakpoint(u32 address)
	{
		Breakpoint breakpoint;
		breakpoint.id = _nextBreakpointId++;
		breakpoint.kind = BreakpointKind::Address;
		breakpoint.address = address;
		breakpoint.verified = true;
		if (const auto location = _debugInfo.locationOf(address); location.has_value())
		{
			breakpoint.file = std::string(location->expansionFile);
			breakpoint.line = location->expansionLine;
		}
		_breakpoints.push_back(std::move(breakpoint));
		return _breakpoints.back().id;
	}

	std::expected<BreakpointId, std::string> DebugSession::addLineBreakpoint(std::string_view file, u32 line)
	{
		if (_debugInfo.isEmpty())
			return std::unexpected("This program was built without debug information, so it has no line table");

		// Deliberately the first address of the line, not merely the lowest: a line occupying
		// several words is only entered at its head.
		const auto address = _debugInfo.firstAddressOfLine(file, line);
		if (!address.has_value())
			return std::unexpected(std::format("No code was emitted for {}:{}", file, line));

		Breakpoint breakpoint;
		breakpoint.id = _nextBreakpointId++;
		breakpoint.kind = BreakpointKind::Line;
		breakpoint.address = address.value();
		breakpoint.file = std::string(file);
		breakpoint.line = line;
		breakpoint.verified = true;
		_breakpoints.push_back(std::move(breakpoint));
		return _breakpoints.back().id;
	}

	std::expected<BreakpointId, std::string> DebugSession::addSymbolBreakpoint(std::string_view symbol)
	{
		if (_debugInfo.isEmpty())
			return std::unexpected("This program was built without debug information, so it has no symbol table");

		const SymbolEntry* entry = _debugInfo.symbolNamed(symbol);
		if (entry == nullptr)
			return std::unexpected(std::format("No symbol named '{}'", symbol));
		if (entry->kind != static_cast<u8>(SymbolKind::Label))
			return std::unexpected(std::format("'{}' is not a label, so there is no code to stop at", symbol));

		Breakpoint breakpoint;
		breakpoint.id = _nextBreakpointId++;
		breakpoint.kind = BreakpointKind::Symbol;
		breakpoint.address = entry->address;
		breakpoint.symbol = std::string(symbol);
		breakpoint.verified = true;
		if (const auto location = _debugInfo.locationOf(entry->address); location.has_value())
		{
			breakpoint.file = std::string(location->expansionFile);
			breakpoint.line = location->expansionLine;
		}
		_breakpoints.push_back(std::move(breakpoint));
		return _breakpoints.back().id;
	}

	bool DebugSession::removeBreakpoint(BreakpointId id)
	{
		const auto it = std::ranges::find(_breakpoints, id, &Breakpoint::id);
		if (it == _breakpoints.end())
			return false;
		_breakpoints.erase(it);
		return true;
	}

	void DebugSession::clearBreakpoints()
	{
		_breakpoints.clear();
	}

	// --- Inspection -----------------------------------------------------------------------------

	RegisterView DebugSession::registers() const
	{
		const vm::ExecutionEngine& engine = _vm->engine();

		RegisterView view;
		for (usize i = 0; i < vm::GeneralPurposeRegisterPool::Count; ++i)
			view.general[i] = engine.registers().getValue(i);
		for (usize i = 0; i < vm::FloatingPointRegisterPool::Count; ++i)
			view.floating[i] = engine.fregisters().getValue(i);

		view.flags = engine.flags().value();
		view.programCounter = engine.programCounter().value();
		view.executedInstructions = engine.executedInstructions();
		view.zero = engine.flags().zero();
		view.sign = engine.flags().sign();
		view.carry = engine.flags().carry();
		view.overflow = engine.flags().overflow();
		view.interruptEnabled = engine.flags().interrupt();
		view.halting = engine.flags().halting();
		view.trap = engine.flags().trap();
		return view;
	}

	std::optional<SourceLocation> DebugSession::currentLocation() const
	{
		return _debugInfo.locationOf(programCounter());
	}

	std::vector<u8> DebugSession::readMemory(u32 address, u32 size) const
	{
		std::vector<u8> out;
		if (size == 0)
			return out;

		const usize memorySize = _vm->memory().size();
		if (address >= memorySize)
			return out;

		const u32 clamped = static_cast<u32>(std::min<usize>(size, memorySize - address));
		out.resize(clamped);
		// Unchecked on purpose: a debugger is allowed to look at the vector table and the BIOS,
		// which are exactly the regions the program itself is kept out of.
		_vm->memory().readBytesUnchecked(vm::Address(address), out);
		return out;
	}

	bool DebugSession::writeMemory(u32 address, std::span<const u8> bytes)
	{
		if (bytes.empty())
			return true;
		if (address >= _vm->memory().size() || address + bytes.size() > _vm->memory().size())
			return false;

		_vm->memory().writeBytesUnchecked(vm::Address(address), bytes);
		return true;
	}

	std::string DebugSession::symbolNameAt(u32 address) const
	{
		// The nearest label at or before the address, which in .text is the subroutine it is in.
		const SymbolEntry* best = nullptr;
		for (const SymbolEntry& symbol : _debugInfo.symbols())
		{
			if (symbol.kind != static_cast<u8>(SymbolKind::Label) || symbol.address > address)
				continue;
			if (best == nullptr || symbol.address > best->address)
				best = &symbol;
		}

		if (best == nullptr)
			return std::format("{:#010x}", address);

		const u32 offset = address - best->address;
		return offset == 0
			? std::string(_debugInfo.symbolName(*best))
			: std::format("{}+{:#x}", _debugInfo.symbolName(*best), offset);
	}

	std::vector<DisassembledInstruction> DebugSession::disassemble(u32 address, u32 before, u32 count) const
	{
		std::vector<DisassembledInstruction> out;
		if (count == 0)
			return out;

		// Exact rather than a guess: every instruction is four bytes, so walking backwards cannot
		// land in the middle of one the way it can on a variable-length encoding.
		const u32 backwards = before * InstructionSize;
		u32 start = address > backwards ? address - backwards : 0;
		start -= start % InstructionSize;

		const u32 total = before + count;
		out.reserve(total);

		for (u32 i = 0; i < total; ++i)
		{
			const u32 at = start + i * InstructionSize;
			if (at + InstructionSize > _vm->memory().size())
				break;

			DisassembledInstruction line;
			line.address = at;
			line.raw = _vm->memory().readInstruction(vm::Address(at)).raw();
			line.text = vm::Disassembler::disassemble(vm::Instruction(line.raw));
			line.location = _debugInfo.locationOf(at);

			// A label sitting exactly on this address, so a listing can head each subroutine.
			for (const SymbolEntry& candidate : _debugInfo.symbols())
			{
				if (candidate.kind == static_cast<u8>(SymbolKind::Label) && candidate.address == at)
				{
					line.symbol = std::string(_debugInfo.symbolName(candidate));
					break;
				}
			}

			out.push_back(std::move(line));
		}

		return out;
	}

	std::string DebugSession::renderVariable(const SymbolEntry& symbol) const
	{
		const ScalarType type = static_cast<ScalarType>(symbol.scalarType);

		if (symbol.kind == static_cast<u8>(SymbolKind::Constant))
		{
			if ((symbol.flags & SymbolFlag::HasValue) == 0)
				return "?";

			const u8 raw[4] = {
				static_cast<u8>(symbol.value & 0xFF),
				static_cast<u8>((symbol.value >> 8) & 0xFF),
				static_cast<u8>((symbol.value >> 16) & 0xFF),
				static_cast<u8>((symbol.value >> 24) & 0xFF)
			};
			return renderScalar(type, raw);
		}

		const usize elementSize = scalarSize(type);
		if (elementSize == 0 || symbol.size == 0)
			return "?";

		const std::vector<u8> bytes = readMemory(symbol.address, symbol.size);
		if (bytes.empty())
			return "<unreadable>";

		// A u8 array is nearly always a string in this language — every message in the tutorial is
		// one — so it reads far better quoted than as sixteen separate numbers.
		if (type == ScalarType::U8 && symbol.elementCount > 1)
		{
			std::string text = "\"";
			for (u8 byte : bytes)
			{
				if (byte == 0)
					break;
				if (byte == '\n') { text += "\\n"; continue; }
				if (byte == '\r') { text += "\\r"; continue; }
				if (byte == '\t') { text += "\\t"; continue; }
				if (byte == '"')  { text += "\\\""; continue; }
				if (byte < 0x20)  { text += std::format("\\x{:02x}", byte); continue; }
				text += static_cast<char>(byte);
			}
			text += '"';
			return text;
		}

		if (symbol.elementCount <= 1)
			return renderScalar(type, bytes);

		std::string text = "[";
		const usize elements = std::min<usize>(symbol.elementCount, bytes.size() / elementSize);
		for (usize i = 0; i < elements; ++i)
		{
			// Long arrays are truncated rather than dumped: a variables view is for reading, and
			// the memory view is there for anyone who wants every byte.
			if (i == 16)
			{
				text += std::format(", ... {} more", elements - i);
				break;
			}
			if (i != 0)
				text += ", ";
			text += renderScalar(type, std::span<const u8>(bytes).subspan(i * elementSize, elementSize));
		}
		text += ']';
		return text;
	}

	std::vector<VariableView> DebugSession::globals() const
	{
		std::vector<VariableView> out;

		for (const SymbolEntry& symbol : _debugInfo.symbols())
		{
			const bool isVariable = symbol.kind == static_cast<u8>(SymbolKind::Variable);
			const bool isConstant = symbol.kind == static_cast<u8>(SymbolKind::Constant);
			if (!isVariable && !isConstant)
				continue;

			VariableView view;
			view.name = std::string(_debugInfo.symbolName(symbol));
			view.address = symbol.address;
			view.size = symbol.size;
			view.isConstant = isConstant;
			view.type = symbol.elementCount > 1
				? std::format("{}[{}]", scalarName(static_cast<ScalarType>(symbol.scalarType)), symbol.elementCount)
				: std::string(scalarName(static_cast<ScalarType>(symbol.scalarType)));
			view.value = renderVariable(symbol);
			out.push_back(std::move(view));
		}

		return out;
	}

	bool DebugSession::setRegister(std::string_view name, u32 value)
	{
		vm::ExecutionEngine& engine = _vm->engine();

		if (name == "pc")
			return setProgramCounter(value);
		if (name == "sp")
			return engine.setRegister(vm::GeneralPurposeRegisterPool::StackPointerIndex, value);
		if (name == "fp")
			return engine.setRegister(vm::GeneralPurposeRegisterPool::FramePointerIndex, value);
		if (name == "lr")
			return engine.setRegister(vm::GeneralPurposeRegisterPool::LinkRegisterIndex, value);

		if (name.size() >= 2 && (name[0] == 'r' || name[0] == 'f'))
		{
			u32 index = 0;
			for (usize i = 1; i < name.size(); ++i)
			{
				if (name[i] < '0' || name[i] > '9')
					return false;
				index = index * 10 + static_cast<u32>(name[i] - '0');
			}

			if (index > 0xFF)
				return false;

			if (name[0] == 'r')
				return engine.setRegister(static_cast<u8>(index), value);

			// A float register is set from the bit pattern, not from a converted integer: the
			// caller has a 32-bit word and only it knows what the word means.
			return engine.setFloatRegister(static_cast<u8>(index), std::bit_cast<f32>(value));
		}

		return false;
	}

	bool DebugSession::setProgramCounter(u32 address)
	{
		if (address % InstructionSize != 0 || address + InstructionSize > _vm->memory().size())
			return false;

		_vm->engine().setProgramCounter(vm::Address(address));
		refreshTopFrame();
		return true;
	}
}

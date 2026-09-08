#pragma once

// A machine you can stop. DebugSession owns a CeresVM and its devices and drives the step loop
// itself instead of handing it to CeresVM::run(), which lets it decide between one instruction and
// the next: whether a breakpoint has been reached, whether a source line has changed, whether the
// caller has asked it to stop.
//
// It knows nothing about JSON, DAP, or terminals. Everything here answers in plain structs, so the
// whole of it can be driven from a test the same way a debugger drives it.

#include "debug_info.h"
#include "expression.h"
#include "history.h"
#include "vm/ceresvm.h"
#include "vm/devices.h"
#include "vm/program.h"
#include <atomic>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ceres::debug
{
	using BreakpointId = u32;

	enum class StopReason : u8
	{
		Entry,        // Stopped before the first instruction, because stopOnEntry was asked for
		Breakpoint,
		Step,         // A step of some kind finished
		Pause,        // Someone called requestPause()
		Halted,       // The machine is in HALT with nothing left to wake it
		Exception,    // A fault or trap was taken
		DataBreakpoint, // A watched range of memory changed
		Exited,       // The program shut the machine down
		StepLimit,    // Ran longer than the caller allowed; nothing is wrong, it just did not finish
		Error,
	};

	std::string_view describe(StopReason reason) noexcept;
	std::string_view describe(vm::InterruptNumber number) noexcept;

	struct StopEvent
	{
		StopReason reason = StopReason::Error;
		u32 address = 0;                 // Program counter where the machine came to rest
		BreakpointId breakpoint = 0;     // Set when reason == Breakpoint
		vm::InterruptNumber exception{}; // Set when reason == Exception
		u32 exceptionAddress = 0;        // The instruction that caused it, not the handler it went to
		BreakpointId dataBreakpoint = 0; // Set when reason == DataBreakpoint
		std::string message;             // Human-readable summary, always filled in
	};

	enum class BreakpointKind : u8
	{
		Address, // A raw address, from the disassembly view
		Line,    // A source line, resolved through the line table
		Symbol,  // A label, for "break on this subroutine"
	};

	struct BreakpointOptions
	{
		// An expression over the machine; empty means unconditional. See expression.h.
		std::string condition;
		// "5" (from the fifth hit on), ">5", ">=5", "==5", "%3" (every third).
		std::string hitCondition;
		// Non-empty makes this a logpoint: {expressions} in it are interpolated, the message is
		// reported, and the program carries on instead of stopping.
		std::string logMessage;
	};

	struct Breakpoint
	{
		BreakpointId id = 0;
		BreakpointKind kind = BreakpointKind::Address;
		u32 address = 0;
		std::string file;   // For Line
		u32 line = 0;       // For Line
		std::string symbol; // For Symbol
		bool verified = false; // False when the location could not be resolved to any address
		// Counts arrivals whose condition held, which is what a hit condition counts against and
		// what an editor shows.
		u32 hitCount = 0;
		BreakpointOptions options;
	};

	// A watch on a range of memory. Only writes are detected, and by comparing the bytes to a
	// snapshot between instructions rather than by trapping the access: the machine has no memory
	// hook, and adding one would put a branch in the hot path of every load and store.
	// What a watch is watching for. Writes are the default because they are what a program does
	// to a variable; reads are asked for by name, since almost everything reads almost everything.
	enum class WatchMode : u8 { Write, Read, ReadWrite };

	struct DataBreakpoint
	{
		BreakpointId id = 0;
		u32 address = 0;
		u32 size = 0;
		WatchMode mode = WatchMode::Write;
		std::string label;      // What the user asked to watch, for the stop message
		std::vector<u8> before; // What it held when it was last looked at, for the stop message
		u32 hitCount = 0;
		// Set by the access observer when the machine touched this range, and read between
		// instructions. A read leaves no trace in memory, so nothing else could find it.
		bool pending = false;
		bool pendingWasWrite = false;
	};

	// One entry of the reconstructed call stack. `reconstructed` is always true and says so on
	// purpose: CALL pushes only a return address and nothing in the machine tracks frames, so this
	// is built by watching instructions go by, not by unwinding.
	struct Frame
	{
		u32 address = 0;        // Program counter inside this frame
		u32 entryAddress = 0;   // Where the frame was entered
		u32 returnAddress = 0;  // Where it will return to; 0 for the outermost frame
		u32 stackPointer = 0;   // Stack pointer as it stood on entry
		std::string name;       // Enclosing symbol, or a synthetic name
		bool isInterruptHandler = false;
		bool reconstructed = true;
		std::optional<SourceLocation> location;
	};

	struct RegisterView
	{
		std::array<u32, vm::GeneralPurposeRegisterPool::Count> general{};
		std::array<f32, vm::FloatingPointRegisterPool::Count> floating{};
		u32 flags = 0;
		u32 programCounter = 0;
		u64 executedInstructions = 0;
		bool zero = false, sign = false, carry = false, overflow = false;
		bool interruptEnabled = false, halting = false, trap = false;
	};

	struct DisassembledInstruction
	{
		u32 address = 0;
		u32 raw = 0;
		std::string text;
		std::optional<SourceLocation> location;
		std::string symbol; // Enclosing label, when the address starts one
	};

	// A global as the debugger sees it: the symbol, plus the bytes currently in memory rendered
	// through the type the assembler recorded for it.
	struct VariableView
	{
		std::string name;
		std::string type;    // "u8[16]", "i32", ...
		std::string value;   // Rendered; a string variable comes back quoted
		u32 address = 0;
		u32 size = 0;
		bool isConstant = false;
	};

	struct LaunchConfig
	{
		// Either one .cres, or one or more .casm files linked together the way `ceres asm` does.
		std::vector<std::filesystem::path> sources;
		usize memorySize = vm::Memory::DefaultSize;
		bool stopOnEntry = true;
		// Recording costs one copy of the machine's memory plus the pages each snapshot dirties,
		// and buys stepping backwards. Worth it by default; a very large --memory is the case
		// where it is not.
		bool recordHistory = true;
		History::Settings history{};
	};

	// How many times each instruction has run. The addresses are every word of .text, whether it
	// ran or not, so a zero is as informative as a large number: it is code nothing reached.
	struct CoverageEntry
	{
		u32 address = 0;
		u64 count = 0;
		std::optional<SourceLocation> location;
	};

	class DebugSession
	{
	public:
		// Nothing here loops forever by accident: every run mode is bounded, so a program stuck in
		// an infinite loop comes back as StepLimit instead of hanging the caller. Generous enough
		// that a real program finishing normally never notices.
		static inline constexpr u64 DefaultStepLimit = 200'000'000;
		// Stepping one source line should not need to grind through a whole program; a line that
		// takes longer than this is a loop the user meant to step over.
		static inline constexpr u64 StepLineLimit = 10'000'000;

		using OutputHandler = std::function<void(std::span<const u8>)>;
		// Where a logpoint's text goes. Separate from OutputHandler because it is the debugger
		// talking, not the program.
		using LogHandler = std::function<void(std::string_view)>;

	private:
		vm::Program _program;
		DebugInfo _debugInfo;
		LaunchConfig _config;

		// Heap-allocated so restart() can throw the whole machine away and build a fresh one
		// without the session itself moving; IOPorts holds raw pointers to the devices.
		std::unique_ptr<vm::CeresVM> _vm;
		std::unique_ptr<vm::TerminalDevice> _terminal;
		std::unique_ptr<vm::TimerDevice> _timer;
		std::unique_ptr<vm::SystemControlDevice> _systemControl;

		std::vector<Breakpoint> _breakpoints;
		std::vector<DataBreakpoint> _dataBreakpoints;
		BreakpointId _nextBreakpointId = 1;

		// Empty optional means every system exception stops the machine, which is the useful
		// default. A list - even an empty one - means exactly those and no others.
		std::optional<std::vector<vm::InterruptNumber>> _exceptionFilters;

		std::vector<Frame> _callStack;

		History _history;
		// Kept alongside the history's snapshots, because the reconstructed call stack cannot be
		// rebuilt from memory: it is inferred from instructions that have already gone past.
		std::deque<std::pair<u64, std::vector<Frame>>> _callStackHistory;

		// One counter per word of .text, indexed by address rather than hashed: incrementing a
		// vector entry costs nothing, and a hash insert per instruction would be felt.
		std::vector<u64> _executionCounts;
		u32 _textStart = 0;

		// Set from any thread; read between instructions.
		std::atomic<bool> _pauseRequested{ false };

		bool _started = false;
		bool _terminated = false;

		OutputHandler _outputHandler;
		LogHandler _logHandler;

		// Filled by the interrupt observer during a step and consumed right after it.
		struct PendingInterrupt
		{
			vm::InterruptNumber number{};
			u32 atAddress = 0;
			bool entered = false;
			bool valid = false;
		};
		PendingInterrupt _lastInterrupt;

	public:
		// Pinned rather than movable, and handed out behind a unique_ptr: the pause flag is an
		// atomic, the devices are registered with IOPorts by raw pointer, and the interrupt
		// observer captures `this`. Any one of those would make a move a trap.
		DebugSession() = delete;
		DebugSession(const DebugSession&) = delete;
		DebugSession(DebugSession&&) = delete;
		~DebugSession() = default;

		DebugSession& operator=(const DebugSession&) = delete;
		DebugSession& operator=(DebugSession&&) = delete;

	private:
		DebugSession(vm::Program&& program, DebugInfo&& debugInfo, const LaunchConfig& config);

	public:
		// Assembles or loads the program, builds the machine, and leaves it at the entry point
		// without running anything. Nothing executes until start().
		static std::expected<std::unique_ptr<DebugSession>, std::string> launch(const LaunchConfig& config);

		// Boots the machine and returns where it came to rest — at the entry point when
		// stopOnEntry, otherwise wherever the first breakpoint or the end of the program is.
		StopEvent start();
		void terminate();
		StopEvent restart();

		bool isRunning() const noexcept { return _started && !_terminated && _vm->isPoweredOn(); }
		bool hasTerminated() const noexcept { return _terminated; }

	public:
		StopEvent stepInstruction();
		StopEvent stepLine();  // Step in: stops on the next source line, wherever it is
		StopEvent stepOver();  // Same, but a CALL is run to completion rather than entered
		StopEvent stepOut();   // Runs until this frame returns
		StopEvent runToAddress(u32 address);
		StopEvent resume(u64 maxInstructions = DefaultStepLimit);

		// Backwards. All of these work by restoring the nearest snapshot and running forward
		// again, which is only possible because the machine is deterministic; see history.h.
		StopEvent stepBackInstruction();
		StopEvent stepBackLine();
		StopEvent reverseContinue();
		StopEvent runToTick(u64 tick);

		u64 currentTick() const noexcept;
		bool canStepBack() const noexcept;
		const History& history() const noexcept { return _history; }

		// Safe from another thread while resume() is running.
		void requestPause() noexcept { _pauseRequested.store(true, std::memory_order_release); }

	public:
		std::expected<BreakpointId, std::string> addLineBreakpoint(std::string_view file, u32 line, BreakpointOptions options = {});
		std::expected<BreakpointId, std::string> addAddressBreakpoint(u32 address, BreakpointOptions options = {});
		std::expected<BreakpointId, std::string> addSymbolBreakpoint(std::string_view symbol, BreakpointOptions options = {});
		bool removeBreakpoint(BreakpointId id);
		void clearBreakpoints();
		std::span<const Breakpoint> breakpoints() const noexcept { return _breakpoints; }

		// `label` is only used to say what changed when it fires; the address and size are what
		// is actually watched.
		std::expected<BreakpointId, std::string> addDataBreakpoint(u32 address, u32 size, std::string label, WatchMode mode = WatchMode::Write);
		bool removeDataBreakpoint(BreakpointId id);
		void clearDataBreakpoints();
		std::span<const DataBreakpoint> dataBreakpoints() const noexcept { return _dataBreakpoints; }

		// Pass nullopt to stop on every system exception, which is the default.
		void setExceptionFilters(std::optional<std::vector<vm::InterruptNumber>> filters);
		bool stopsOn(vm::InterruptNumber number) const noexcept;

	public:
		RegisterView registers() const;
		std::span<const Frame> callStack() const noexcept { return _callStack; }

		// The stack walked through the frame pointers, when the assembler recorded that the
		// function the machine is in opens one. Every frame it returns is exact rather than
		// reconstructed. Empty when the innermost function has no frame, which is when there is
		// nothing to walk and the reconstruction is all there is.
		std::vector<Frame> unwindCallStack() const;
		std::optional<SourceLocation> currentLocation() const;
		u32 programCounter() const;

		std::vector<u8> readMemory(u32 address, u32 size) const;
		bool writeMemory(u32 address, std::span<const u8> bytes);

		// `before` instructions of context ahead of `address`, then `count` from it. Reading
		// backwards is exact rather than a guess because every instruction is four bytes.
		std::vector<DisassembledInstruction> disassemble(u32 address, u32 before, u32 count) const;

		std::vector<VariableView> globals() const;

		// Every word of .text with how many times it has run. Zeroes are the point: they are the
		// code no run has reached.
		std::vector<CoverageEntry> coverage() const;

		bool setRegister(std::string_view name, u32 value);
		bool setProgramCounter(u32 address);

		// Watches, hover, conditions and logpoint interpolation all come through here.
		std::expected<EvalResult, std::string> evaluate(std::string_view expression) const;

	public:
		// Bytes the program writes to the terminal's output port. Set before start(), or the
		// first few will have gone to stdout already.
		void setOutputHandler(OutputHandler handler);
		void setLogHandler(LogHandler handler) { _logHandler = std::move(handler); }
		void pushInput(std::string_view text);

		const DebugInfo& debugInfo() const noexcept { return _debugInfo; }
		const vm::Program& program() const noexcept { return _program; }
		vm::CeresVM& machine() noexcept { return *_vm; }

	private:
		void attachDevices();

		// One instruction, plus everything that has to happen around it: watching for the call and
		// return that maintain the shadow stack, and for an interrupt being taken.
		StopEvent stepOnce();

		// Shared by every run mode. Steps until `shouldStop` says so or something else intervenes.
		StopEvent runUntil(const std::function<bool()>& shouldStop, u64 maxInstructions);

		StopEvent makeStop(StopReason reason) const;
		Breakpoint* breakpointAt(u32 address);

		// Condition, then hit count, then log message: a logpoint whose condition is false should
		// not log, and a hit condition counts only the hits the condition allowed through.
		bool shouldStopAt(Breakpoint& breakpoint);
		bool hitConditionSatisfied(const Breakpoint& breakpoint) const;

		// Compares every watched range to its snapshot. Returns the one that changed, if any.
		void noteAccess(vm::AccessKind kind, u32 address, u32 size);
		DataBreakpoint* checkDataBreakpoints();
		void refreshDataSnapshots();

		// Restores the nearest snapshot at or before `tick` and replays up to it, with recording,
		// breakpoints and output all suppressed - this is ground the program has already covered
		// and the user has already seen.
		bool replayTo(u64 tick);
		// Walks the whole reachable history looking for the last moment before now that satisfies
		// `matches`, then goes there. Shared by reverse-continue and step-back-a-line.
		StopEvent scanBackFor(const std::function<bool()>& matches, std::string_view whatFor);

		void updateCallStack(vm::Instruction executed, u32 pcBefore, u32 pcAfter, u32 spAfter, bool wasHalted);
		void resetCallStack();
		void refreshTopFrame();

		std::string symbolNameAt(u32 address) const;
		std::string renderVariable(const SymbolEntry& symbol) const;
	};
}

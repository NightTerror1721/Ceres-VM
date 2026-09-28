#pragma once

#include "memory.h"
#include "vram.h"
#include "mmio_bus.h"
#include "fault_reason.h"
#include <ceres/core/isa/cycles.h>
#include "interrupt_controller.h"
#include "mmu.h"
#include <ceres/core/isa/address.h>
#include <ceres/core/isa/fregisters.h>
#include <ceres/core/isa/instructions.h>
#include <ceres/core/isa/wide.h>
#include <ceres/core/isa/interrupts.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <span>
#include <vector>
#include <limits>
#include <cmath>
#include <functional>
#include <optional>
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define CERES_VM_SSE2_SQRT 1
#endif

namespace ceres::vm
{
	// Which way a load or a store went, for a watchpoint that wants to know.
	enum class AccessKind : u8 { Read, Write };

	class ExecutionEngine
	{
	private:
		GeneralPurposeRegisterPool _registers;
		FloatingPointRegisterPool _fregisters;
		Address _pc; // Program Counter (PC)
		FlagRegister _flags; // Flags register
		Memory& _memory;
		Vram& _vram;
		MmioBus& _mmioBus;
		InterruptController& _interrupts;
		Mmu _mmu;

		// Set the instant a translation or an alignment check redirects the PC into a fault handler,
		// and checked by advancePC() so the handler that was mid-instruction cannot then walk the PC
		// past that redirect. Reset once per step(), before fetch(). Existing faults (alignment, the
		// text-segment write guard) already returned early before their own advancePC() and never
		// needed this; it is what makes every *other* load/store handler - the ones that call
		// read<T>()/write<T>() unconditionally, like LDRB - safe now that either can fault too.
		bool _faulted = false;

		// Instructions retired since the last reset. The timer already counts in executed
		// instructions rather than wall clock, so this is the machine's own notion of time and
		// the only clock a debugger can step against deterministically.
		u64 _executedInstructions = 0;
		// The CPU clock: cycles spent since the machine started, by the table of plan/v2 SPEC 3.2 (cycles.h).
		u64 _cycles = 0;
		// When the next scheduled device event is due (Scheduler::nextCycle, kept here by the scheduler).
		u64 _nextEvent = NoScheduledEvent;

		// The lowest address the stack may grow down to. Until a program is loaded this is all the
		// machine can defend - the vector table and the BIOS - which is what it was defending
		// before: a runaway stack ate the program's own text for as many megabytes as it took to
		// reach 0x400, and only then said so. loadProgram lowers it to the end of the loaded image.
		u32 _stackLimit = static_cast<u32>(Memory::UnrestrictedSegmentStartValue);
		// The lowest the limit may be put: the end of the loaded image. A program moves the limit up
		// over its heap (setProgramStackLimit, through the system-control device) and never below this,
		// and a reset puts it back here.
		u32 _stackFloor = static_cast<u32>(Memory::UnrestrictedSegmentStartValue);

		// How many interrupts are being serviced, and what the program's own stack pointer was when
		// the first one arrived. Nested interrupts stay on the system stack; only the outermost one
		// switches, and only it switches back.
		u32 _interruptDepth = 0;
		u32 _savedStackPointer = 0;

		// STI takes effect one instruction late: the instruction right after it runs before any
		// user interrupt can be delivered. That is what makes "sti; halt" a single indivisible
		// step - an interrupt that arrives between the two used to be serviced before the halt ran,
		// so the machine then slept with nothing left to wake it. Set by STI, spent by the next step().
		bool _interruptShadow = false;

		// HALT waits for an event, the way ARM's WFE does: a request raised since the machine last
		// woke - taken or not, masked by CLI or with no handler bound - ends the wait, and one raised
		// before the HALT has run keeps it from sleeping at all. This is the raise count
		// (InterruptController::raiseCount) at the last wake, so "raised since" is a comparison and a
		// request that stays pending while masked wakes a HALT once, not every time. A program can so
		// sleep with interrupts masked - arm a timer, CLI, HALT, look at the clock - and needs no
		// handler, and a wake-up that comes between its last look and the HALT is not lost.
		u64 _raisesConsumed = 0;

		// Set when a fault found no stack left to save its state on and the machine stopped (Trap and
		// Halting together): that halt is for good, and no request ends it. Its own flag, because Trap
		// alone also stays set after a benign division by zero, and a program that then sleeps with CLI
		// and HALT must still be woken.
		bool _stoppedForGood = false;

		// The data address and the access of the last memory fault - an unaligned access, a store into
		// .text or below it, a page fault - so a fault handler can say what the faulting instruction was
		// doing and where, not only where it was. What the system-control device's FaultAddressRegister and
		// FaultAccessRegister read. The access is FaultAccess in bits 0-7 and the size in bytes in 8-31 -
		// wider than a byte, as a block instruction's chunk runs up to a page (0 when the MMU faulted on the
		// first page of an access whose size it does not know).
		u32 _faultAddress = 0;
		u32 _faultAccess = 0;
		FaultReason _faultReason = FaultReason::None;
		// --strict-mmio: an offset a device does not declare faults instead of reading 0 (plan/v2 SPEC 5.1).
		bool _strictMmio = false;

		// Whether a division by zero raises the DivisionByZero interrupt. Off unless the program
		// switches it on through the system control device, so a program written without it keeps
		// the old behaviour: the Trap flag is set and the destination is left alone.
		bool _divisionFaults = false;

		// Whether a float division by zero gives IEEE's infinity or NaN rather than trapping (FeatureIeeeDivide).
		bool _ieeeDivide = false;

		// The loaded program's own text, which nothing a correct program does ever writes to. An
		// empty range means no program is loaded and there is nothing to protect. A store into it
		// used to simply take effect, so a lost pointer rewrote an instruction that had not run
		// yet and the machine went wrong somewhere else entirely.
		u32 _textStart = 0;
		u32 _textEnd = 0;

		// One counter per instruction word of .text, and null unless a profile was asked for.
		// The machine already counts in executed instructions rather than wall clock, so this is a
		// profile that comes out the same on every run - which a real machine cannot offer.
		std::vector<u64> _executionCounts;
		u64* _executionCountsData = nullptr;
		std::vector<u64> _cycleCounts;              // what each word cost, cycle by cycle, alongside the counts
		u64* _cycleCountsData = nullptr;

		// Empty unless a debugger is attached. A watchpoint that compares snapshots between
		// instructions cannot see a read at all, nor a write that puts back the value that was
		// already there; this sees the access itself. The cost when nobody is watching is one
		// predictable branch per load and store.
		std::function<void(AccessKind, u32, u32)> _accessObserver;

		// Empty unless a debugger is attached; see setInterruptObserver.
		std::function<void(InterruptNumber, Address, bool)> _interruptObserver;


	public:
		explicit ExecutionEngine(Memory& memory, Vram& vram, MmioBus& mmioBus, InterruptController& interrupts) :
			_memory(memory), _vram(vram), _mmioBus(mmioBus), _interrupts(interrupts)
		{
			_mmioBus.scheduler().bindTo(&_cycles, &_nextEvent);   // the devices' events are kept in this CPU's cycles
		}
		ExecutionEngine(const ExecutionEngine&) = delete;
		ExecutionEngine(ExecutionEngine&&) = delete;
		~ExecutionEngine() = default;

		ExecutionEngine& operator=(const ExecutionEngine&) = delete;
		ExecutionEngine& operator=(ExecutionEngine&&) = delete;

	public:
		void reset() noexcept;
		void step() noexcept;

	public:
		// Read-only observability. Tests (and any future debugger) need to inspect the machine
		// state between steps; nothing here can mutate it.
		constexpr const GeneralPurposeRegisterPool& registers() const noexcept { return _registers; }
		constexpr const FloatingPointRegisterPool& fregisters() const noexcept { return _fregisters; }
		constexpr const FlagRegister& flags() const noexcept { return _flags; }
		constexpr Address programCounter() const noexcept { return _pc; }
		constexpr bool isHalted() const noexcept { return _flags.halting(); }
		constexpr u64 executedInstructions() const noexcept { return _executedInstructions; }
		constexpr u64 cycles() const noexcept { return _cycles; }
		constexpr u32 stackLimit() const noexcept { return _stackLimit; }
		constexpr u32 interruptDepth() const noexcept { return _interruptDepth; }
		// Where the program's own stack ends and the system stack begins.
		u32 systemStackFloor() const noexcept { return static_cast<u32>(_memory.size() - Memory::SystemStackSize); }
		constexpr u32 textStart() const noexcept { return _textStart; }
		constexpr u32 textEnd() const noexcept { return _textEnd; }
		std::span<const u64> executionCounts() const noexcept { return _executionCounts; }
		// The cycles each instruction word has cost, over all its executions (an interrupt's entry is nobody's).
		std::span<const u64> cycleCounts() const noexcept { return _cycleCounts; }

		// A program builds its own page tables in plain memory before PGON, and a test or debugger
		// wants to build them from outside too - see docs/27-Virtual-Memory-and-Paging.md.
		Mmu& mmu() noexcept { return _mmu; }
		const Mmu& mmu() const noexcept { return _mmu; }

	public:
		// Write access, for a debugger: setting a register from the editor's variables view,
		// jumping to the cursor, restoring a snapshot. Kept apart from the read-only block above
		// and out of the private helpers the instruction handlers use, so a call from inside the
		// machine itself reads as the mistake it would be. Each one rejects an out-of-range index
		// rather than trusting a caller that is, by definition, outside the machine.
		bool setRegister(u8 index, u32 value) noexcept
		{
			if (index >= GeneralPurposeRegisterPool::Count)
				return false;
			_registers.setValue(index, value);
			return true;
		}

		bool setFloatRegister(u8 index, f32 value) noexcept
		{
			if (index >= FloatingPointRegisterPool::Count)
				return false;
			_fregisters.setValue(index, value);
			return true;
		}

		// The register's bits exactly, NaN payloads and all: what a snapshot puts back.
		bool setFloatRegisterBits(u8 index, u32 bits) noexcept
		{
			if (index >= FloatingPointRegisterPool::Count)
				return false;
			_fregisters.setBits(index, bits);
			return true;
		}

		// Set by the loader once it knows where the image ends. A reset deliberately leaves it
		// alone: the same program is still in memory, so the same ground is still worth guarding.
		// Told about every load and store the program performs, with the address and the width.
		// Instruction fetch is deliberately not reported: it is not an access the program made.
		void setAccessObserver(std::function<void(AccessKind, u32, u32)> observer) noexcept
		{
			_accessObserver = std::move(observer);
		}

		// The loader's: the end of the image it placed, which is both the limit and the floor below which a
		// program cannot put it.
		void setStackLimit(u32 lowestAddress) noexcept { _stackLimit = lowestAddress; _stackFloor = lowestAddress; }
		// The program's own, from the system-control device's StackLimitRegister: the top of its heap, so a
		// stack that runs down into it is a StackOverflow rather than silently rewritten allocations. Never
		// below the image; above the stack pointer it makes the next push overflow, which is the program's
		// to avoid.
		void setProgramStackLimit(u32 lowestAddress) noexcept { _stackLimit = lowestAddress > _stackFloor ? lowestAddress : _stackFloor; }

		// Also the loader's to set, and also kept across a reset. Pass an empty range to lift the
		// protection, which is what a machine with no program loaded has.
		void setTextRange(u32 start, u32 end) noexcept { _textStart = start; _textEnd = end; }

		// Starts counting how often each instruction word runs. Call after the text range is set;
		// the counters are indexed off it.
		void enableProfiling()
		{
			_executionCounts.assign(_textEnd > _textStart ? (_textEnd - _textStart) / Instruction::Size : 0, 0);
			_executionCountsData = _executionCounts.empty() ? nullptr : _executionCounts.data();
			_cycleCounts.assign(_executionCounts.size(), 0);
			_cycleCountsData = _cycleCounts.empty() ? nullptr : _cycleCounts.data();
		}

		void setFlags(FlagRegister flags) noexcept { _flags = flags; }
		void setDivisionFaults(bool enabled) noexcept { _divisionFaults = enabled; }
		constexpr bool divisionFaults() const noexcept { return _divisionFaults; }
		void setIeeeDivide(bool enabled) noexcept { _ieeeDivide = enabled; }
		constexpr bool ieeeDivide() const noexcept { return _ieeeDivide; }
		void setProgramCounter(Address address) noexcept { _pc = address; }
		// Only for restoring a snapshot: the machine's clock has to go back with the rest of it,
		// or a restored timer would fire against a count that never rewound.
		void setExecutedInstructions(u64 count) noexcept { _executedInstructions = count; }
		void setCycles(u64 cycles) noexcept { _cycles = cycles; }

		enum class FaultAccess : u8 { None = 0, Read = 1, Write = 2, Execute = 3 };
		u32 faultAddress() const noexcept { return _faultAddress; }
		u32 faultAccess() const noexcept { return _faultAccess; }
		// Why: a FaultReason (plan/v2 SPEC 5.4), what SystemControl's FaultReason register reports.
		u32 faultReason() const noexcept { return static_cast<u32>(_faultReason); }

		void setStrictMmio(bool strict) noexcept { _strictMmio = strict; }
		bool strictMmio() const noexcept { return _strictMmio; }
		// Only for restoring a snapshot: a rewind to before a fault must not still report it.
		void setFaultRegisters(u32 address, u32 access, u32 reason = 0) noexcept { _faultAddress = address; _faultAccess = access; _faultReason = static_cast<FaultReason>(reason); }
		// Whether the machine stopped for want of stack (_stoppedForGood). A debugger keeps it in its
		// snapshots: it decides whether a HALT can ever be woken.
		bool stoppedForGood() const noexcept { return _stoppedForGood; }
		void setStoppedForGood(bool stopped) noexcept { _stoppedForGood = stopped; }
		// Whether a request has been raised since the machine last woke, so the next HALT will not
		// sleep (_raisesConsumed). A debugger keeps it in its snapshots, since it decides what a HALT does.
		bool hasWakeEvent() const noexcept { return _interrupts.raiseCount() != _raisesConsumed; }
		void setWakeEvent(bool pending) noexcept
		{
			const u64 raises = _interrupts.raiseCount();
			_raisesConsumed = pending && raises != 0 ? raises - 1 : raises;
		}

	public:
		// Told about every interrupt the machine takes, with the program counter as it stood when
		// the interrupt fired — which for a fault is the instruction that caused it, and is
		// otherwise unrecoverable once the dispatch has redirected the PC into the handler.
		// `entered` distinguishes a handler actually being run from a request that was masked,
		// had no handler, or could not be delivered for want of stack.
		//
		// Must not throw: it is called from triggerInterrupt, which is noexcept.
		using InterruptObserver = std::function<void(InterruptNumber number, Address atPc, bool entered)>;
		void setInterruptObserver(InterruptObserver observer) noexcept { _interruptObserver = std::move(observer); }
		void clearInterruptObserver() noexcept { _interruptObserver = nullptr; }

	private:
		inline void handleReset() noexcept { reset(); }
		void handleHalt() noexcept;
		// One step of a halted machine: the clock jumps to the next device event, nothing executes.
		void haltedStep(u64 raisesSeen) noexcept;
		// The fetch and execute of a step whose word is being profiled (enableProfiling), at `index` in .text.
		neverinline void stepCounted(usize index) noexcept;
		void handleTrap() noexcept;

		// True when a handler was entered; false when the request was ignored (masked, or no handler bound)
		// or the machine stopped for want of stack to save its state on.
		bool triggerInterrupt(InterruptNumber interruptNumber) noexcept;

		inline void execute(const Instruction instruction) noexcept
		{
			(this->*InstructionHandlers[static_cast<u8>(instruction.opcode())])(instruction);
		}

	private:
		// The one MMU chokepoint every read<T>/write<T> call and fetch() share. Paging off is the
		// fast, common path: the address comes back unchanged and nothing about Memory has to know
		// paging exists at all. Paging on and a translation failure both go through triggerInterrupt
		// here, exactly like the alignment and text-segment checks already do, so PageFault behaves
		// like every other fault this engine raises rather than like a new kind of failure.
		forceinline std::optional<Address> translate(Address address, MmuAccess access) noexcept
		{
			if (!_flags.get<ExecutionFlag::Paging>()) [[likely]]
				return address;

			// The null page and the BIOS (below 0x400), the system stack (the top SystemStackSize bytes
			// of RAM) and the devices (0xFF000000 up) are VM-owned that no program's page table
			// describes - they stay physical whether or not paging is on, the same way they are already
			// reached through the unchecked path rather than the checked one. This is not just
			// convenience: without it, a page fault taken while the system stack itself happened to be
			// unmapped would recurse into dispatching the very fault it is trying to save a frame for,
			// and a program that forgot to map its own fault vectors could never even reach the BIOS's
			// default handler to fail safely. Everything else is virtual, above the end of RAM too: a
			// program can put a page at 0x80000000 on a 16 MiB machine.
			const u32 raw = address.value();
			if (raw < Memory::UnrestrictedSegmentStartValue || (raw >= systemStackFloor() && raw < _memory.size()) ||
				raw >= MmioBus::BaseValue)
				return address;

			if (const auto physical = _mmu.translate(_memory, address, access))
				return physical;

			noteFault(address, access == MmuAccess::Read ? FaultAccess::Read : access == MmuAccess::Write ? FaultAccess::Write : FaultAccess::Execute, 0);
			triggerInterrupt(InterruptNumber::PageFault);
			return std::nullopt;
		}

		forceinline Instruction fetch() noexcept
		{
			const auto physical = translate(_pc, MmuAccess::Execute);
			if (!physical.has_value())
				return Instruction(0); // Never executed: step() checks _faulted and skips execute().
			const u32 p = physical->value();
			if (p <= _memory.size() - Instruction::Size) [[likely]]
				return Instruction(_memory.readBacked<Instruction::RawType, 0>(p));
			return fetchOutsideRam(p);
		}

		// An instruction from the VRAM runs as one from the RAM does. Anywhere else there is nothing to run: past the
		// RAM, past the VRAM or in an empty region, a MemoryFault with the reason the data access would have (SPEC
		// 5.4), and in the device window one with MmioWidth, since a device register is only read by a 32-bit load.
		Instruction fetchOutsideRam(u32 physical) noexcept
		{
			if (_vram.backs(physical, Instruction::Size))
				return Instruction(_vram.read<Instruction::RawType>(physical - Vram::BaseValue));
			const FaultReason reason = MmioBus::contains(Address(physical)) ? FaultReason::MmioWidth : unbackedReason(physical);
			noteFault(_pc, FaultAccess::Execute, Instruction::Size, reason);
			triggerInterrupt(InterruptNumber::MemoryFault);
			return Instruction(0);
		}

		// Why a physical address outside the RAM, the VRAM and the device window has nothing behind it (SPEC 2).
		FaultReason unbackedReason(u32 physical) const noexcept
		{
			if (physical < fmt::MemoryMap::RamLimitValue)
				return FaultReason::OutOfRam;
			if (Vram::inWindow(physical))
				return FaultReason::OutOfVram;
			return FaultReason::Unmapped;
		}

		// A load or a store that reached no memory: MemoryFault with the reason, the access and its size.
		void unbacked(Address address, u32 physical, FaultAccess access, u32 size) noexcept
		{
			noteFault(address, access, size, unbackedReason(physical));
			triggerInterrupt(InterruptNumber::MemoryFault);
		}
		// A no-op once this instruction has already faulted: triggerInterrupt has redirected the PC
		// to the handler, and a handler still mid-execution after that must not then walk it forward
		// again. Every handler that does not check for a fault explicitly - most of them - relies on
		// this to stay correct now that read<T>/write<T> can fault too.
		forceinline void advancePC() noexcept { if (!_faulted) _pc += Instruction::SizeInBytes; }

		forceinline u32 getReg(u8 index) const noexcept { return _registers.getValue(index); }
		forceinline void setReg(u8 index, u32 value) noexcept { _registers.setValue(index, value); }

		forceinline f32 getFloatReg(u8 index) const noexcept { return _fregisters.getValue(index); }
		forceinline void setFloatReg(u8 index, f32 value) noexcept { _fregisters.setValue(index, value); }
		// A float register's bits, for everything that moves a value rather than computing one: fmov, the loads and
		// stores, push and pop. They never pass through a host float, so a NaN keeps its payload and its signal.
		forceinline u32 getFloatBits(u8 index) const noexcept { return _fregisters.getBits(index); }
		forceinline void setFloatBits(u8 index, u32 bits) noexcept { _fregisters.setBits(index, bits); }

		forceinline u32 sp() const noexcept { return _registers.getValue<GeneralPurposeRegisterPool::StackPointerIndex>(); }
		forceinline void sp(u32 value) noexcept { _registers.setValue<GeneralPurposeRegisterPool::StackPointerIndex>(value); }

		forceinline u32 fp() const noexcept { return _registers.getValue<GeneralPurposeRegisterPool::FramePointerIndex>(); }
		forceinline void fp(u32 value) noexcept { _registers.setValue<GeneralPurposeRegisterPool::FramePointerIndex>(value); }

		forceinline u32 at() const noexcept { return _registers.getValue<GeneralPurposeRegisterPool::AssemblerTempIndex>(); }
		forceinline void at(u32 value) noexcept { _registers.setValue<GeneralPurposeRegisterPool::AssemblerTempIndex>(value); }

		// A halfword or word access has to sit on a boundary of its own size. Byte accesses never
		// fault. Returns false when the access is misaligned, having already raised the fault.
		forceinline void noteFault(Address address, FaultAccess access, u32 size, FaultReason reason = FaultReason::None) noexcept
		{
			_faultAddress = address.value();
			_faultAccess = static_cast<u32>(access) | (size << 8);
			_faultReason = reason;
		}

		// Whether an access to `address` would land on a device: the window itself (never translated), or a page a
		// program mapped onto it. Asked only when an access has already faulted, so it only looks (Mmu::probe).
		bool reachesDevice(Address address) const noexcept
		{
			if (MmioBus::contains(address))
				return true;
			if (!_flags.get<ExecutionFlag::Paging>())
				return false;
			const auto physical = _mmu.probe(_memory, address);
			return physical.has_value() && MmioBus::contains(*physical);
		}

		template <typename T>
		forceinline bool checkAlignment(Address address, FaultAccess access = FaultAccess::Read) noexcept
		{
			if constexpr (sizeof(T) <= 1)
			{
				return true;
			}
			else
			{
				if ((address.value() % sizeof(T)) == 0)
					return true;

				noteFault(address, access, static_cast<u32>(sizeof(T)), reachesDevice(address) ? FaultReason::MmioWidth : FaultReason::Alignment);
				triggerInterrupt(InterruptNumber::AlignmentFault);
				return false;
			}
		}

		// `chargeMemory` false is the second word of a 64-bit access (load64): memory takes the two words as one access
		// and has charged it once, while a device is two accesses and charges both.
		template <typename T> requires (Integral<T> || FloatingPoint<T>) && (sizeof(T) <= sizeof(u32))
		forceinline T read(Address address, bool chargeMemory = true) noexcept
		{
			if (_accessObserver) [[unlikely]]
				_accessObserver(AccessKind::Read, address.value(), static_cast<u32>(sizeof(T)));

			const auto physical = translate(address, MmuAccess::Read);
			if (!physical.has_value())
				return T{};

			// The physical map (plan/v2 SPEC 2): below 0xA0000000 the RAM, the only region a program touches all the
			// time, for one comparison with the end of the RAM; everything else after the one comparison that sends
			// it away. The null page and the BIOS read as zero, as they always have.
			const u32 p = physical->value();
			if (p < Vram::BaseValue) [[likely]]
			{
				if (p > _memory.size() - sizeof(T)) [[unlikely]]
				{
					unbacked(address, p, FaultAccess::Read, static_cast<u32>(sizeof(T)));
					return T{};
				}
				if (chargeMemory)
					_cycles += isa::cycles::RamAccess;
				return _memory.readBacked<T, Memory::UnrestrictedSegmentStartValue>(p);
			}
			return readOutsideRam<T>(address, p, chargeMemory);
		}

		template <typename T> requires (Integral<T> || FloatingPoint<T>) && (sizeof(T) <= sizeof(u32))
		T readOutsideRam(Address address, u32 p, bool chargeMemory) noexcept
		{
			if (_vram.backs(p, sizeof(T)))
			{
				if (chargeMemory)
					_cycles += isa::cycles::VramAccess;
				return _vram.read<T>(p - Vram::BaseValue);
			}

			// The top 16 MiB of physical address space is the devices' - see MmioBus. A physical address there
			// routes here, whether it arrived as-is (paging off) or as the frame a page table happened to map to
			// (paging on): mapping a device's window into a program's own virtual space is then just an ordinary
			// page table entry.
			const Address physical = Address(p);
			if (MmioBus::contains(physical))
			{
				// A device register takes an aligned 32-bit access and nothing else (plan/v2 SPEC 5.1); the
				// alignment was already checked. Decided at compile time, so RAM accesses pay nothing for it.
				if constexpr (sizeof(T) != sizeof(u32))
				{
					noteFault(address, FaultAccess::Read, static_cast<u32>(sizeof(T)), FaultReason::MmioWidth);
					triggerInterrupt(InterruptNumber::MemoryFault);
					return T{};
				}
				if (_strictMmio && !_mmioBus.declares(physical)) [[unlikely]]
				{
					noteFault(address, FaultAccess::Read, static_cast<u32>(sizeof(T)), FaultReason::MmioUndeclared);
					triggerInterrupt(InterruptNumber::MemoryFault);
					return T{};
				}
				_cycles += isa::cycles::MmioAccess;
				if constexpr (FloatingPoint<T>)
					return std::bit_cast<T>(_mmioBus.read(physical));
				else
					return static_cast<T>(_mmioBus.read(physical));
			}

			unbacked(address, p, FaultAccess::Read, static_cast<u32>(sizeof(T)));
			return T{};
		}

		// Returns false when the write would land in the program's own text, or in the vector table
		// or BIOS below it, having already raised MemoryFault. The caller must abort the instruction,
		// exactly as for a misaligned access. Memory refuses those low stores by itself, but it does so
		// silently: without the fault a stray pointer to the null page looks like a store that worked.
		forceinline bool checkWritable(Address address, u32 size) noexcept
		{
			const u64 base = address.value();
			if (base < Memory::UnrestrictedSegmentStartValue ||
				(_textEnd > _textStart && base < _textEnd && base + size > _textStart))
			{
				noteFault(address, FaultAccess::Write, size);
				triggerInterrupt(InterruptNumber::MemoryFault);
				return false;
			}
			return true;
		}

		template <typename T> requires (Integral<T> || FloatingPoint<T>) && (sizeof(T) <= sizeof(u32))
		forceinline void write(Address address, T value, bool chargeMemory = true) noexcept
		{
			if (_accessObserver) [[unlikely]]
				_accessObserver(AccessKind::Write, address.value(), static_cast<u32>(sizeof(T)));

			const auto physical = translate(address, MmuAccess::Write);
			if (!physical.has_value())
				return;

			// The same routing as read(). A store below 0x400 is refused by checkWritable before it gets here, and
			// one that gets here anyway (a push, an interrupt's frame) is dropped, as it always was.
			const u32 p = physical->value();
			if (p < Vram::BaseValue) [[likely]]
			{
				if (p > _memory.size() - sizeof(T)) [[unlikely]]
				{
					unbacked(address, p, FaultAccess::Write, static_cast<u32>(sizeof(T)));
					return;
				}
				if (chargeMemory)
					_cycles += isa::cycles::RamAccess;
				_memory.writeBacked<T, Memory::UnrestrictedSegmentStartValue>(p, value);
				return;
			}
			writeOutsideRam<T>(address, p, value, chargeMemory);
		}

		template <typename T> requires (Integral<T> || FloatingPoint<T>) && (sizeof(T) <= sizeof(u32))
		void writeOutsideRam(Address address, u32 p, T value, bool chargeMemory) noexcept
		{
			if (_vram.backs(p, sizeof(T)))
			{
				if (chargeMemory)
					_cycles += isa::cycles::VramAccess;
				_vram.write<T>(p - Vram::BaseValue, value);
				return;
			}

			const Address physical = Address(p);
			if (MmioBus::contains(physical))
			{
				if constexpr (sizeof(T) != sizeof(u32))
				{
					noteFault(address, FaultAccess::Write, static_cast<u32>(sizeof(T)), FaultReason::MmioWidth);
					triggerInterrupt(InterruptNumber::MemoryFault);
					return;
				}
				if (_strictMmio && !_mmioBus.declares(physical)) [[unlikely]]
				{
					noteFault(address, FaultAccess::Write, static_cast<u32>(sizeof(T)), FaultReason::MmioUndeclared);
					triggerInterrupt(InterruptNumber::MemoryFault);
					return;
				}
				_cycles += isa::cycles::MmioAccess;
				if constexpr (FloatingPoint<T>)
					_mmioBus.write(physical, std::bit_cast<u32>(value));
				else
					_mmioBus.write(physical, static_cast<u32>(value));
				return;
			}

			unbacked(address, p, FaultAccess::Write, static_cast<u32>(sizeof(T)));
		}

		template <ExecutionFlag Flag>
		forceinline void flag(bool value) noexcept { _flags.setFlag<Flag>(value); }
		forceinline void carry(bool value) noexcept { _flags.setFlag<ExecutionFlag::Carry>(value); }
		forceinline void zero(bool value) noexcept { _flags.setFlag<ExecutionFlag::Zero>(value); }
		forceinline void sign(bool value) noexcept { _flags.setFlag<ExecutionFlag::Sign>(value); }
		forceinline void overflow(bool value) noexcept { _flags.setFlag<ExecutionFlag::Overflow>(value); }
		forceinline void interrupt(bool value) noexcept { _flags.setFlag<ExecutionFlag::Interrupt>(value); }
		forceinline void halting(bool value) noexcept { _flags.setFlag<ExecutionFlag::Halting>(value); }
		forceinline void trap(bool value) noexcept { _flags.setFlag<ExecutionFlag::Trap>(value); }

		template <ExecutionFlag Flag>
		forceinline bool flag() noexcept { return _flags.get<Flag>(); }
		forceinline bool carry() noexcept { return _flags.carry(); }
		forceinline bool zero() noexcept { return _flags.zero(); }
		forceinline bool sign() noexcept { return _flags.sign(); }
		forceinline bool overflow() noexcept { return _flags.overflow(); }
		forceinline bool interrupt() noexcept { return _flags.interrupt(); }
		forceinline bool halting() noexcept { return _flags.halting(); }
		forceinline bool trap() noexcept { return _flags.trap(); }

		// The stack grows down from the end of memory. Below the unrestricted segment it would run
		// into the BIOS and the interrupt vectors; the checked write already refuses those addresses,
		// so without this the overflow was silent and execution carried on with garbage.
		forceinline bool hasStackRoom(u32 bytes) const noexcept
		{
			// A handler runs on the system stack, whose floor is the top of the program's own.
			const u32 floor = _interruptDepth > 0 ? systemStackFloor() : _stackLimit;
			return sp() >= floor + bytes;
		}

		forceinline bool hasStackData(u32 bytes) const noexcept
		{
			// The ceiling is where the stack started: the top of memory for a handler, and the
			// floor of the system stack for the program, which is where its own stack begins.
			const u64 ceiling = _interruptDepth > 0
				? static_cast<u64>(_memory.size())
				: static_cast<u64>(systemStackFloor());
			return static_cast<u64>(sp()) + bytes <= ceiling;
		}

		// Returns false when the push faulted. The caller must abort the instruction: the fault
		// handler has already redirected the PC, and finishing the instruction would overwrite it.
		template <typename T> requires (Integral<T> || FloatingPoint<T>) && (sizeof(T) <= sizeof(u32))
		forceinline bool push(T value) noexcept
		{
			if (!hasStackRoom(sizeof(T)))
			{
				triggerInterrupt(InterruptNumber::StackOverflow);
				return false;
			}

			// sp moves before the write lands, so a write that page-faults has to put it back: the
			// fault handler's IRET re-runs this same PUSH from the top, and a second decrement of an
			// sp that never actually got written would leave a live word of stack skipped forever.
			const u32 savedSp = sp();
			sp(savedSp - sizeof(T));
			if constexpr (FloatingPoint<T>)
				write<u32>(Address(sp()), std::bit_cast<u32>(value));
			else
				write(Address(sp()), value);

			if (_faulted)
			{
				sp(savedSp);
				return false;
			}
			return true;
		}

		template <typename T> requires (Integral<T> || FloatingPoint<T>) && (sizeof(T) <= sizeof(u32))
		// Empty when the pop faulted; see push() for why the caller has to bail out.
		forceinline std::optional<T> pop() noexcept
		{
			// Popping past the top of memory means the stack is unbalanced: a RET without its CALL,
			// or one POP too many. Same fault class as an overflow.
			if (!hasStackData(sizeof(T)))
			{
				triggerInterrupt(InterruptNumber::StackOverflow);
				return std::nullopt;
			}

			if constexpr (FloatingPoint<T>)
			{
				const T value = std::bit_cast<T>(read<u32>(Address(sp())));
				if (_faulted)
					return std::nullopt; // sp is untouched: the fault handler's IRET retries this pop from scratch.
				sp(sp() + sizeof(T));
				return value;
			}
			else
			{
				const T value = read<T>(Address(sp()));
				if (_faulted)
					return std::nullopt;
				sp(sp() + sizeof(T));
				return value;
			}
		}

		// The same flag rule the bitwise operations use: Zero and Sign from the result, Carry and
		// Overflow cleared, because none of these can carry or overflow. ABS is the exception and
		// sets its own Overflow.
		forceinline void executeResult(const u8 regDest, const u32 result) noexcept
		{
			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeAbs(const u8 regDest, const u32 value) noexcept
		{
			const i32 signedValue = static_cast<i32>(value);
			// |INT_MIN| is not representable: the result is INT_MIN again, and Overflow says so
			// rather than the machine pretending it answered the question.
			const bool overflowed = value == 0x80000000u;
			const u32 result = overflowed ? value : static_cast<u32>(signedValue < 0 ? -signedValue : signedValue);

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(overflowed);

			setReg(regDest, result);
			advancePC();
		}

		static forceinline constexpr u32 rotateLeft(u32 value, u32 amount) noexcept
		{
			amount &= 31u;
			return amount == 0 ? value : ((value << amount) | (value >> (32u - amount)));
		}

		// One bit per category, RISC-V fclass-style but collapsed to a single NaN bit rather than
		// separating quiet from signaling - the standard library does not portably distinguish them.
		// Bit 0: -Infinity  Bit 1: -Normal  Bit 2: -Subnormal  Bit 3: -Zero
		// Bit 4: +Zero      Bit 5: +Subnormal  Bit 6: +Normal  Bit 7: +Infinity  Bit 8: NaN
		template <FloatingPoint T>
		static forceinline u32 classifyFloat(T value) noexcept
		{
			const bool negative = std::signbit(value);
			switch (std::fpclassify(value))
			{
				case FP_NAN:       return 1u << 8;
				case FP_INFINITE:  return negative ? (1u << 0) : (1u << 7);
				case FP_ZERO:      return negative ? (1u << 3) : (1u << 4);
				case FP_SUBNORMAL: return negative ? (1u << 2) : (1u << 5);
				default:           return negative ? (1u << 1) : (1u << 6); // FP_NORMAL
			}
		}

		// The square root rounded once, as IEEE 754 asks, whatever the build. std::sqrt is not enough: unoptimised, GCC
		// calls the C library, and MinGW's works in x87 extended precision and rounds a second time, so sqrt(0x3FEF...FF)
		// came out 1.0 in Debug and right in Release. On x86 the SSE2 instruction is taken directly; elsewhere the
		// library's sqrt is already correctly rounded.
		static forceinline f64 squareRoot(const f64 value) noexcept
		{
#ifdef CERES_VM_SSE2_SQRT
			return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(value)));
#else
			return std::sqrt(value);
#endif
		}
		static forceinline f32 squareRoot(const f32 value) noexcept
		{
#ifdef CERES_VM_SSE2_SQRT
			return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(value)));
#else
			return std::sqrt(value);
#endif
		}

		forceinline void executeAdd(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			const u64 result = static_cast<u64>(a) + static_cast<u64>(b);

			zero((result & 0xFFFFFFFF) == 0);
			sign((result & 0x80000000) != 0);
			carry(result > 0xFFFFFFFF);
			overflow((~(a ^ b) & (a ^ static_cast<u32>(result))) & 0x80000000);

			setReg(regDest, static_cast<u32>(result));
			advancePC();
		}

		forceinline void executeFloatAdd(const u8 regDest, const f32 a, const f32 b) noexcept
		{
			const f32 result = a + b;
			const auto resultClass = std::fpclassify(result);

			zero(resultClass == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(resultClass == FP_INFINITE && std::fpclassify(a) != FP_INFINITE && std::fpclassify(b) != FP_INFINITE);

			setFloatReg(regDest, result);
			advancePC();
		}

		forceinline void executeSub(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			const u64 result = static_cast<u64>(a) - static_cast<u64>(b);

			zero((result & 0xFFFFFFFF) == 0);
			sign((result & 0x80000000) != 0);
			carry(a < b);
			overflow(((a ^ b) & (a ^ static_cast<u32>(result))) & 0x80000000);

			setReg(regDest, static_cast<u32>(result));
			advancePC();
		}

		forceinline void executeFloatSub(const u8 regDest, const f32 a, const f32 b) noexcept
		{
			const f32 result = a - b;
			const auto resultClass = std::fpclassify(result);

			zero(resultClass == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(resultClass == FP_INFINITE && std::fpclassify(a) != FP_INFINITE && std::fpclassify(b) != FP_INFINITE);

			setFloatReg(regDest, result);
			advancePC();
		}

        forceinline void executeSignedMul(const u8 regDest, const i32 a, const i32 b) noexcept
		{
			constexpr i64 max32 = static_cast<i64>(std::numeric_limits<i32>::max());
			constexpr i64 min32 = static_cast<i64>(std::numeric_limits<i32>::min());

			const i64 result = static_cast<i64>(a) * static_cast<i64>(b);
			const i32 result32 = static_cast<i32>(result);
			const bool didOverflow = (result > max32 || result < min32);

			zero(result32 == 0);
			sign(result32 < 0);
			carry(didOverflow);
			overflow(didOverflow);

			setReg(regDest, static_cast<u32>(result32));
			advancePC();
		}

		forceinline void executeUnsignedMul(const u8 regDest, const u32 a, const u32 b) noexcept
		{
            const u64 result = static_cast<u64>(a) * static_cast<u64>(b);
			const u32 result32 = static_cast<u32>(result);
			const bool didCarry = result > 0xFFFFFFFF;

			zero(result32 == 0);
			sign((result32 & 0x80000000) != 0);
			carry(didCarry);
			overflow(didCarry);

			setReg(regDest, result32);
			advancePC();
		}

		forceinline void executeFloatMul(const u8 regDest, const f32 a, const f32 b) noexcept
		{
			const f32 result = a * b;
			const auto resultClass = std::fpclassify(result);

			zero(resultClass == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(resultClass == FP_INFINITE && std::fpclassify(a) != FP_INFINITE && std::fpclassify(b) != FP_INFINITE);

			setFloatReg(regDest, result);
			advancePC();
		}

		// What every division-shaped instruction does when the divisor is zero. The destination is
		// left as it was either way. It advances first, like TRAP, so a handler that returns lands
		// on the instruction after the division rather than repeating it forever.
		forceinline void divisionByZero() noexcept
		{
			if (_divisionFaults)
			{
				advancePC();
				triggerInterrupt(InterruptNumber::DivisionByZero);
				return;
			}
			trap(true);
			advancePC();
		}

		forceinline void executeSignedDiv(const u8 regDest, const i32 a, const i32 b) noexcept
		{
			if (b == 0)
			{
				divisionByZero();
				return;
			}

			const i64 result = static_cast<i64>(a) / static_cast<i64>(b);
			const i32 result32 = static_cast<i32>(result);

			zero(result32 == 0);
			sign(result32 < 0);
			carry(false);

			const bool didOverflow = (b == -1 && a == std::numeric_limits<i32>::min());
			overflow(didOverflow);

			setReg(regDest, static_cast<u32>(result32));
			advancePC();
		}

		forceinline void executeUnsignedDiv(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			if (b == 0)
			{
				divisionByZero();
				return;
			}

			const u32 result = a / b;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		// x / +-0 as IEEE 754 has it: NaN for 0/0 and NaN/0, else an infinity with the sign of the two signs together.
		// Worked out rather than divided, so the host's own floating-point settings play no part.
		static f32 ieeeDivideByZero(const f32 a, const f32 b) noexcept
		{
			if (std::isnan(a) || a == 0.0f)
				return std::numeric_limits<f32>::quiet_NaN();
			const bool negative = std::signbit(a) != std::signbit(b);
			return negative ? -std::numeric_limits<f32>::infinity() : std::numeric_limits<f32>::infinity();
		}

		forceinline void executeFloatDiv(const u8 regDest, const f32 a, const f32 b) noexcept
		{
			if (b == 0.0f && !_ieeeDivide)
			{
				divisionByZero();
				return;
			}

			const f32 result = b == 0.0f ? ieeeDivideByZero(a, b) : a / b;
			const auto resultClass = std::fpclassify(result);

			zero(resultClass == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(resultClass == FP_INFINITE && std::fpclassify(a) != FP_INFINITE && std::fpclassify(b) != FP_INFINITE);

			setFloatReg(regDest, result);
			advancePC();
		}

		// The float sibling of executeUnsignedMod/executeSignedMod: same trap-on-zero convention as
		// every other division-shaped instruction (see executeFloatDiv), even though IEEE fmod(x, 0)
		// is well-defined as NaN - unless FeatureIeeeDivide is on, and then it is that NaN.
		forceinline void executeFloatMod(const u8 regDest, const f32 a, const f32 b) noexcept
		{
			if (b == 0.0f && !_ieeeDivide)
			{
				divisionByZero();
				return;
			}

			const f32 result = b == 0.0f ? std::numeric_limits<f32>::quiet_NaN() : std::fmod(a, b);
			const auto resultClass = std::fpclassify(result);

			zero(resultClass == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(false);

			setFloatReg(regDest, result);
			advancePC();
		}

		forceinline void executeSignedMod(const u8 regDest, const i32 a, const i32 b) noexcept
		{
			if (b == 0)
			{
				divisionByZero();
				return;
			}

			// remainder computed in wider type to avoid UB
			const i64 result = static_cast<i64>(a) % static_cast<i64>(b);
			const i32 result32 = static_cast<i32>(result);

			zero(result32 == 0);
			sign(result32 < 0);
			carry(false);
			overflow(false);

			setReg(regDest, static_cast<u32>(result32));
			advancePC();
		}

		forceinline void executeUnsignedMod(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			if (b == 0)
			{
				divisionByZero();
				return;
			}

			const u32 result = a % b;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeFloatNeg(const u8 regDest, const f32 a) noexcept
		{
			const f32 result = -a;
			const auto resultClass = std::fpclassify(result);

			zero(resultClass == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(resultClass == FP_INFINITE && std::fpclassify(a) != FP_INFINITE);

			setFloatReg(regDest, result);
			advancePC();
		}

		forceinline void executeAnd(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			const u32 result = a & b;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeOr(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			const u32 result = a | b;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeXor(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			const u32 result = a ^ b;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeNot(const u8 regDest, const u32 a) noexcept
		{
			const u32 result = ~a;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry(false);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeShl(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			if (b == 0)
			{
				// No shift, so no flags are affected
				setReg(regDest, a);
				advancePC();
				return;
			}

			const u32 shiftAmount = b & 0x1F; // Only consider the lower 5 bits for shift amount
			const u32 result = a << shiftAmount;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry((a << (shiftAmount - 1)) & 0x80000000);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeShr(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			if (b == 0)
			{
				// No shift, so no flags are affected
				setReg(regDest, a);
				advancePC();
				return;
			}

			const u32 shiftAmount = b & 0x1F; // Only consider the lower 5 bits for shift amount
			const u32 result = a >> shiftAmount;

			zero(result == 0);
			sign((result & 0x80000000) != 0);
			carry((a >> (shiftAmount - 1)) & 0x1);
			overflow(false);

			setReg(regDest, result);
			advancePC();
		}

		forceinline void executeSar(const u8 regDest, const u32 a, const u32 b) noexcept
		{
			if (b == 0)
			{
				// No shift, so no flags are affected
				setReg(regDest, a);
				advancePC();
				return;
			}

			const u32 shiftAmount = b & 0x1F; // Only consider the lower 5 bits for shift amount
			const i32 signedA = static_cast<i32>(a);
			const i32 result = signedA >> shiftAmount;

			zero(result == 0);
			sign(result < 0);
			carry((signedA >> (shiftAmount - 1)) & 0x1);
			overflow(false);

			setReg(regDest, static_cast<u32>(result));
			advancePC();
		}

		template <ExecutionFlag Flag>
		forceinline void executeJumpIfFlag(const Instruction inst) noexcept
		{
			if (flag<Flag>())
			{
				_pc += inst.simm24().signedValue();
				_cycles += isa::cycles::TakenBranchExtra;
			}
			else
				advancePC();
		}

		template <ExecutionFlag Flag>
		forceinline void executeJumpRegIfFlag(const Instruction inst) noexcept
		{
			if (flag<Flag>())
			{
				_pc = Address(getReg(inst.rs()));
				_cycles += isa::cycles::TakenBranchExtra;
			}
			else
				advancePC();
		}

		// The comparison jumps read two flags, so they cannot go through the templates above.
		// CMP leaves: Zero = (a == b), Sign = sign bit of (a - b), Carry = (a < b) unsigned,
		// Overflow = signed overflow of (a - b). Signed ordering is Sign against Overflow;
		// unsigned ordering is Carry, with Zero separating < from <=.
		forceinline void executeJumpIf(const Instruction inst, bool condition) noexcept
		{
			if (condition)
			{
				_pc += inst.simm24().signedValue();
				_cycles += isa::cycles::TakenBranchExtra;
			}
			else
				advancePC();
		}

		forceinline void executeJumpRegIf(const Instruction inst, bool condition) noexcept
		{
			if (condition)
			{
				_pc = Address(getReg(inst.rs()));
				_cycles += isa::cycles::TakenBranchExtra;
			}
			else
				advancePC();
		}

		forceinline bool signedLess() noexcept { return flag<ExecutionFlag::Sign>() != flag<ExecutionFlag::Overflow>(); }
		forceinline bool isEqual() noexcept { return flag<ExecutionFlag::Zero>(); }
		forceinline bool unsignedBelow() noexcept { return flag<ExecutionFlag::Carry>(); }

		template <ExecutionFlag Flag>
		forceinline void executeJumpIfNotFlag(const Instruction inst) noexcept
		{
			if (!flag<Flag>())
			{
				_pc += inst.simm24().signedValue();
				_cycles += isa::cycles::TakenBranchExtra;
			}
			else
				advancePC();
		}

		template <ExecutionFlag Flag>
		forceinline void executeJumpRegIfNotFlag(const Instruction inst) noexcept
		{
			if (!flag<Flag>())
			{
				_pc = Address(getReg(inst.rs()));
				_cycles += isa::cycles::TakenBranchExtra;
			}
			else
				advancePC();
		}

	private:
		forceinline void NOP(const Instruction inst) noexcept { advancePC(); }
		forceinline void HALT(const Instruction inst) noexcept { handleHalt(); }
		// Both advance first: the return address has to be the instruction after them, or IRET
		// would land back on the trap and loop forever.
		forceinline void TRAP(const Instruction inst) noexcept { advancePC(); triggerInterrupt(InterruptNumber::Trap); }
		forceinline void RESET(const Instruction inst) noexcept { triggerInterrupt(InterruptNumber::Reset); }
		// There are InterruptNumberCount vectors; a larger number would read the BIOS above the table as a
		// handler address, so it is an illegal instruction, reported at the INT itself.
		forceinline void INT(const Instruction inst) noexcept
		{
			if (inst.imm8() >= isa::InterruptNumberCount) [[unlikely]]
			{
				illegal(FaultReason::BadSubfield);
				return;
			}
			advancePC();
			triggerInterrupt(static_cast<InterruptNumber>(inst.imm8()));
		}
		// Without these the interrupt flag could never be set, so every user interrupt was
		// unreachable: triggerInterrupt drops numbers >= 16 while the flag is clear.
		forceinline void CLI(const Instruction inst) noexcept { _flags.clear<ExecutionFlag::Interrupt>(); advancePC(); }
		forceinline void STI(const Instruction inst) noexcept { _flags.set<ExecutionFlag::Interrupt>(); _interruptShadow = true; advancePC(); }

		// Memory Management Unit. See docs/27-Virtual-Memory-and-Paging.md.
		forceinline void MTP(const Instruction inst) noexcept { _mmu.setPtbr(getReg(inst.rs())); advancePC(); }
		forceinline void MFP(const Instruction inst) noexcept { setReg(inst.rd(), _mmu.ptbr()); advancePC(); }
		forceinline void PGON(const Instruction inst) noexcept { _flags.set<ExecutionFlag::Paging>(); advancePC(); }
		forceinline void PGOFF(const Instruction inst) noexcept { _flags.clear<ExecutionFlag::Paging>(); advancePC(); }
		forceinline void INVLPG(const Instruction inst) noexcept { _mmu.invalidate(Address(getReg(inst.rs()))); advancePC(); }
		forceinline void FLPG(const Instruction inst) noexcept { _mmu.invalidateAll(); advancePC(); }
		forceinline void MFPF(const Instruction inst) noexcept { setReg(inst.rd(), _mmu.faultAddress().value()); advancePC(); }

		// Puts the program's own stack pointer back once the outermost handler is done with it.
		forceinline void leaveInterrupt() noexcept
		{
			if (_interruptDepth == 0)
				return;

			--_interruptDepth;
			if (_interruptDepth == 0)
				sp(_savedStackPointer);
		}

		forceinline void IRET(const Instruction inst) noexcept
		{
			// triggerInterrupt pushes flags and then the PC, so the PC is on top and has to come off
			// first. Popping them the other way round restored the flags word as the program counter:
			// an interrupt taken while halted resumed at address 0x30, the flag bits themselves.
			const u64 iretStart = _cycles;
			const auto newPC = pop<u32>();
			if (!newPC.has_value())
				return;
			const auto newFlags = pop<u32>();
			if (!newFlags.has_value())
				return;

			// triggerInterrupt pushes the PC of the instruction that had not run yet, so returning
			// means restoring it exactly. Advancing here skipped that instruction, which is invisible
			// for a software interrupt (INT advances before trapping) and wrong for everything else.
			_pc = Address(*newPC);
			_flags = FlagRegister(*newFlags);

			// After the pops, so the handler's own two words come off the system stack first. A
			// handler that pushed more than it popped is forgiven by this rather than corrupting
			// the stack of the program it interrupted.
			leaveInterrupt();
			_cycles = iretStart + isa::cycles::IretCycles;
		}

		forceinline void ADD(const Instruction inst) noexcept { executeAdd(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void ADDI(const Instruction inst) noexcept { executeAdd(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void ADDC(const Instruction inst) noexcept { executeAdd(inst.rd(), getReg(inst.rs()), getReg(inst.rt()) + (carry() ? 1 : 0)); }
		forceinline void ADDCI(const Instruction inst) noexcept { executeAdd(inst.rd(), getReg(inst.rs()), inst.imm16() + (carry() ? 1 : 0)); }
		forceinline void FADD(const Instruction inst) noexcept { executeFloatAdd(inst.fd(), getFloatReg(inst.fs()), getFloatReg(inst.ft())); }
		forceinline void SUB(const Instruction inst) noexcept { executeSub(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void SUBI(const Instruction inst) noexcept { executeSub(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void SUBC(const Instruction inst) noexcept { executeSub(inst.rd(), getReg(inst.rs()), getReg(inst.rt()) + (carry() ? 1 : 0)); }
		forceinline void SUBCI(const Instruction inst) noexcept { executeSub(inst.rd(), getReg(inst.rs()), inst.imm16() + (carry() ? 1 : 0)); }
		forceinline void FSUB(const Instruction inst) noexcept { executeFloatSub(inst.fd(), getFloatReg(inst.fs()), getFloatReg(inst.ft())); }
		forceinline void MUL(const Instruction inst) noexcept { executeUnsignedMul(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void MULI(const Instruction inst) noexcept { executeUnsignedMul(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void IMUL(const Instruction inst) noexcept { executeSignedMul(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void IMULI(const Instruction inst) noexcept { executeSignedMul(inst.rd(), getReg(inst.rs()), inst.simm16()); }
		forceinline void FMUL(const Instruction inst) noexcept { executeFloatMul(inst.fd(), getFloatReg(inst.fs()), getFloatReg(inst.ft())); }
		forceinline void DIV(const Instruction inst) noexcept { executeUnsignedDiv(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void DIVI(const Instruction inst) noexcept { executeUnsignedDiv(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void IDIV(const Instruction inst) noexcept { executeSignedDiv(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void IDIVI(const Instruction inst) noexcept { executeSignedDiv(inst.rd(), getReg(inst.rs()), inst.simm16()); }
		forceinline void FDIV(const Instruction inst) noexcept { executeFloatDiv(inst.fd(), getFloatReg(inst.fs()), getFloatReg(inst.ft())); }
		forceinline void MOD(const Instruction inst) noexcept { executeUnsignedMod(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void MODI(const Instruction inst) noexcept { executeUnsignedMod(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void IMOD(const Instruction inst) noexcept { executeSignedMod(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void IMODI(const Instruction inst) noexcept { executeSignedMod(inst.rd(), getReg(inst.rs()), inst.simm16()); }
		forceinline void FMOD(const Instruction inst) noexcept { executeFloatMod(inst.fd(), getFloatReg(inst.fs()), getFloatReg(inst.ft())); }
		forceinline void FNEG(const Instruction inst) noexcept { executeFloatNeg(inst.fd(), getFloatReg(inst.fs())); }

		forceinline void MULH(const Instruction inst) noexcept
		{
			const u64 product = static_cast<u64>(getReg(inst.rs())) * static_cast<u64>(getReg(inst.rt()));
			executeResult(inst.rd(), static_cast<u32>(product >> 32));
		}
		forceinline void IMULH(const Instruction inst) noexcept
		{
			const i64 product = static_cast<i64>(static_cast<i32>(getReg(inst.rs()))) *
				static_cast<i64>(static_cast<i32>(getReg(inst.rt())));
			executeResult(inst.rd(), static_cast<u32>(static_cast<u64>(product) >> 32));
		}
		forceinline void ABS(const Instruction inst) noexcept { executeAbs(inst.rd(), getReg(inst.rs())); }
		forceinline void MIN(const Instruction inst) noexcept { executeResult(inst.rd(), std::min(getReg(inst.rs()), getReg(inst.rt()))); }
		forceinline void MINI(const Instruction inst) noexcept { executeResult(inst.rd(), std::min(getReg(inst.rs()), static_cast<u32>(inst.imm16()))); }
		forceinline void IMIN(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(std::min(static_cast<i32>(getReg(inst.rs())), static_cast<i32>(getReg(inst.rt()))))); }
		forceinline void IMINI(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(std::min(static_cast<i32>(getReg(inst.rs())), static_cast<i32>(inst.simm16())))); }
		forceinline void MAX(const Instruction inst) noexcept { executeResult(inst.rd(), std::max(getReg(inst.rs()), getReg(inst.rt()))); }
		forceinline void MAXI(const Instruction inst) noexcept { executeResult(inst.rd(), std::max(getReg(inst.rs()), static_cast<u32>(inst.imm16()))); }
		forceinline void IMAX(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(std::max(static_cast<i32>(getReg(inst.rs())), static_cast<i32>(getReg(inst.rt()))))); }
		forceinline void IMAXI(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(std::max(static_cast<i32>(getReg(inst.rs())), static_cast<i32>(inst.simm16())))); }
		// Light-touch flags, the way AND/OR/XOR set Zero/Sign and always clear Carry/Overflow: there
		// is no carry or overflow to report from picking one of two values that already existed.
		forceinline void executeFloatMinMax(const u8 regDest, const f32 result) noexcept
		{
			zero(std::fpclassify(result) == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(false);
			setFloatReg(regDest, result);
			advancePC();
		}
		forceinline void FMIN(const Instruction inst) noexcept { executeFloatMinMax(inst.fd(), std::fmin(getFloatReg(inst.fs()), getFloatReg(inst.ft()))); }
		forceinline void FMAX(const Instruction inst) noexcept { executeFloatMinMax(inst.fd(), std::fmax(getFloatReg(inst.fs()), getFloatReg(inst.ft()))); }
		forceinline void CLZ(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(std::countl_zero(getReg(inst.rs())))); }
		forceinline void CTZ(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(std::countr_zero(getReg(inst.rs())))); }
		forceinline void POPCNT(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(std::popcount(getReg(inst.rs())))); }
		forceinline void BSWAP(const Instruction inst) noexcept { executeResult(inst.rd(), std::byteswap(getReg(inst.rs()))); }
		forceinline void ROL(const Instruction inst) noexcept { executeResult(inst.rd(), rotateLeft(getReg(inst.rs()), getReg(inst.rt()))); }
		forceinline void ROLI(const Instruction inst) noexcept { executeResult(inst.rd(), rotateLeft(getReg(inst.rs()), inst.imm16())); }
		forceinline void ROR(const Instruction inst) noexcept { executeResult(inst.rd(), rotateLeft(getReg(inst.rs()), 32u - (getReg(inst.rt()) & 31u))); }
		forceinline void RORI(const Instruction inst) noexcept { executeResult(inst.rd(), rotateLeft(getReg(inst.rs()), 32u - (static_cast<u32>(inst.imm16()) & 31u))); }
		forceinline void SXTB(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(static_cast<i32>(static_cast<i8>(getReg(inst.rs()) & 0xFFu)))); }
		forceinline void SXTH(const Instruction inst) noexcept { executeResult(inst.rd(), static_cast<u32>(static_cast<i32>(static_cast<i16>(getReg(inst.rs()) & 0xFFFFu)))); }
		forceinline void FSQRT(const Instruction inst) noexcept { setFloatReg(inst.fd(), squareRoot(getFloatReg(inst.fs()))); advancePC(); }
		forceinline void FABS(const Instruction inst) noexcept { setFloatReg(inst.fd(), std::fabs(getFloatReg(inst.fs()))); advancePC(); }
		// Rounding, sign injection and the multiply-accumulate a software math library needs for
		// polynomial evaluation. None of these touch the flags register, the same as FSQRT/FABS
		// above: they hand back an unambiguous float and there is nothing a flag would add.
		forceinline void FROUND(const Instruction inst) noexcept { setFloatReg(inst.fd(), std::nearbyint(getFloatReg(inst.fs()))); advancePC(); }
		forceinline void FFLOOR(const Instruction inst) noexcept { setFloatReg(inst.fd(), std::floor(getFloatReg(inst.fs()))); advancePC(); }
		forceinline void FCEIL(const Instruction inst) noexcept { setFloatReg(inst.fd(), std::ceil(getFloatReg(inst.fs()))); advancePC(); }
		forceinline void FTRUNC(const Instruction inst) noexcept { setFloatReg(inst.fd(), std::trunc(getFloatReg(inst.fs()))); advancePC(); }
		forceinline void FCOPYSIGN(const Instruction inst) noexcept { setFloatReg(inst.fd(), std::copysign(getFloatReg(inst.fs()), getFloatReg(inst.ft()))); advancePC(); }
		// fd is read as the accumulator as well as written, so this is exactly FADD's flag/rounding
		// behaviour applied to (fd, fs * ft) - reusing executeFloatAdd instead of duplicating it.
		forceinline void FMA(const Instruction inst) noexcept { executeFloatAdd(inst.fd(), getFloatReg(inst.fd()), getFloatReg(inst.fs()) * getFloatReg(inst.ft())); }
		forceinline void FCLASS(const Instruction inst) noexcept { setReg(inst.rd(), classifyFloat(getFloatReg(inst.fs()))); advancePC(); }
		// Estimates in name only: a software-interpreted VM has no cycle cost to save by answering
		// approximately, so these give an exact reciprocal rather than faking the low precision a
		// real FPU's lookup-table hardware would produce. They keep the trap-on-zero convention the
		// rest of the divide family uses, since 1/0 is exactly the case that family already guards -
		// while FeatureIeeeDivide is off; with it on they give IEEE's infinity.
		forceinline void FRECIPE(const Instruction inst) noexcept
		{
			const f32 value = getFloatReg(inst.fs());
			if (value == 0.0f && !_ieeeDivide)
			{
				divisionByZero();
				return;
			}
			setFloatReg(inst.fd(), value == 0.0f ? ieeeDivideByZero(1.0f, value) : 1.0f / value);
			advancePC();
		}
		forceinline void FRSQRTE(const Instruction inst) noexcept
		{
			const f32 value = getFloatReg(inst.fs());
			if (value == 0.0f && !_ieeeDivide)
			{
				divisionByZero();
				return;
			}
			setFloatReg(inst.fd(), value == 0.0f ? ieeeDivideByZero(1.0f, value) : 1.0f / squareRoot(value));   // 1/sqrt(-0) is -inf
			advancePC();
		}

		forceinline void AND(const Instruction inst) noexcept { executeAnd(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void ANDI(const Instruction inst) noexcept { executeAnd(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void OR(const Instruction inst) noexcept { executeOr(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void ORI(const Instruction inst) noexcept { executeOr(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void XOR(const Instruction inst) noexcept { executeXor(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void XORI(const Instruction inst) noexcept { executeXor(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void NOT(const Instruction inst) noexcept { executeNot(inst.rd(), getReg(inst.rs())); }
		forceinline void SHL(const Instruction inst) noexcept { executeShl(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void SHLI(const Instruction inst) noexcept { executeShl(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void SHR(const Instruction inst) noexcept { executeShr(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void SHRI(const Instruction inst) noexcept { executeShr(inst.rd(), getReg(inst.rs()), inst.imm16()); }
		forceinline void SAR(const Instruction inst) noexcept { executeSar(inst.rd(), getReg(inst.rs()), getReg(inst.rt())); }
		forceinline void SARI(const Instruction inst) noexcept { executeSar(inst.rd(), getReg(inst.rs()), inst.imm16()); }

		forceinline void MOV(const Instruction inst) noexcept { setReg(inst.rd(), getReg(inst.rs())); advancePC(); }
		forceinline void FMOV(const Instruction inst) noexcept { setFloatBits(inst.fd(), getFloatBits(inst.fs())); advancePC(); }
		forceinline void LI(const Instruction inst) noexcept { setReg(inst.rd(), inst.imm16()); advancePC(); }
		forceinline void LUI(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(inst.imm16()) << 16u); advancePC(); }
		// A memory displacement is signed: `[fp - 8]` has to reach eight bytes below the base, not
		// 65528 above it. Widened through i32 and folded back to u32 so the addition wraps the way
		// a 32-bit address space does.
		static forceinline constexpr u32 displacement(const Instruction inst) noexcept
		{
			return static_cast<u32>(static_cast<i32>(inst.simm16()));
		}

		forceinline void LDR(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rs()) + displacement(inst);
			if (!checkAlignment<u32>(address))
				return;
			loaded(inst.rd(), read<u32>(address));
		}
		forceinline void LDRB(const Instruction inst) noexcept { loaded(inst.rd(), read<u8>(getReg(inst.rs()) + displacement(inst))); }
		forceinline void LDRH(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rs()) + displacement(inst);
			if (!checkAlignment<u16>(address))
				return;
			loaded(inst.rd(), read<u16>(address));
		}
		forceinline void LDRSB(const Instruction inst) noexcept { loaded(inst.rd(), static_cast<u32>(read<i8>(getReg(inst.rs()) + displacement(inst)))); }
		forceinline void LDRSH(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rs()) + displacement(inst);
			if (!checkAlignment<i16>(address))
				return;
			loaded(inst.rd(), static_cast<u32>(read<i16>(address)));
		}
		forceinline void FLDR(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rs()) + displacement(inst);
			if (!checkAlignment<f32>(address))
				return;
			loadedFloat(inst.fd(), read<u32>(address));
		}
		forceinline void STR(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rd()) + displacement(inst);
			if (!checkAlignment<u32>(address, FaultAccess::Write) || !checkWritable(address, sizeof(u32)))
				return;
			write<u32>(address, getReg(inst.rs()));
			advancePC();
		}
		forceinline void STRB(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rd()) + displacement(inst);
			if (!checkWritable(address, sizeof(u8)))
				return;
			write<u8>(address, static_cast<u8>(getReg(inst.rs())));
			advancePC();
		}
		forceinline void STRH(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rd()) + displacement(inst);
			if (!checkAlignment<u16>(address, FaultAccess::Write) || !checkWritable(address, sizeof(u16)))
				return;
			write<u16>(address, static_cast<u16>(getReg(inst.rs())));
			advancePC();
		}
		forceinline void FSTR(const Instruction inst) noexcept
		{
			const Address address = getReg(inst.rd()) + displacement(inst);
			if (!checkAlignment<f32>(address, FaultAccess::Write) || !checkWritable(address, sizeof(f32)))
				return;
			write<u32>(address, getFloatBits(inst.fs()));
			advancePC();
		}
		// Indexed forms. The address is two registers added at run time, so there is no
		// displacement to range-check and no ADD to write before every access.
		// A loaded value into its register - unless the load faulted into a handler: the handler, and the retry after
		// its iret, must find the registers as the instruction did, or `ldr r1, [r1]` would retry from address 0.
		forceinline void loaded(u8 dest, u32 value) noexcept
		{
			if (_faulted)
				return;
			setReg(dest, value);
			advancePC();
		}
		forceinline void loadedFloat(u8 dest, u32 bits) noexcept
		{
			if (_faulted)
				return;
			setFloatBits(dest, bits);
			advancePC();
		}

		forceinline Address indexed(const Instruction inst) const noexcept { return Address(getReg(inst.rs()) + getReg(inst.rt())); }
		forceinline Address indexedStore(const Instruction inst) const noexcept { return Address(getReg(inst.rd()) + getReg(inst.rt())); }

		forceinline void LDRX(const Instruction inst) noexcept
		{
			const Address address = indexed(inst);
			if (!checkAlignment<u32>(address))
				return;
			loaded(inst.rd(), read<u32>(address));
		}
		forceinline void LDRBX(const Instruction inst) noexcept { loaded(inst.rd(), read<u8>(indexed(inst))); }
		forceinline void LDRHX(const Instruction inst) noexcept
		{
			const Address address = indexed(inst);
			if (!checkAlignment<u16>(address))
				return;
			loaded(inst.rd(), read<u16>(address));
		}
		forceinline void LDRSBX(const Instruction inst) noexcept { loaded(inst.rd(), static_cast<u32>(read<i8>(indexed(inst)))); }
		forceinline void LDRSHX(const Instruction inst) noexcept
		{
			const Address address = indexed(inst);
			if (!checkAlignment<i16>(address))
				return;
			loaded(inst.rd(), static_cast<u32>(read<i16>(address)));
		}
		forceinline void FLDRX(const Instruction inst) noexcept
		{
			const Address address = indexed(inst);
			if (!checkAlignment<f32>(address))
				return;
			loadedFloat(inst.fd(), read<u32>(address));
		}
		forceinline void STRX(const Instruction inst) noexcept
		{
			const Address address = indexedStore(inst);
			if (!checkAlignment<u32>(address, FaultAccess::Write) || !checkWritable(address, sizeof(u32)))
				return;
			write<u32>(address, getReg(inst.rs()));
			advancePC();
		}
		forceinline void STRBX(const Instruction inst) noexcept
		{
			const Address address = indexedStore(inst);
			if (!checkWritable(address, sizeof(u8)))
				return;
			write<u8>(address, static_cast<u8>(getReg(inst.rs())));
			advancePC();
		}
		forceinline void STRHX(const Instruction inst) noexcept
		{
			const Address address = indexedStore(inst);
			if (!checkAlignment<u16>(address, FaultAccess::Write) || !checkWritable(address, sizeof(u16)))
				return;
			write<u16>(address, static_cast<u16>(getReg(inst.rs())));
			advancePC();
		}
		forceinline void FSTRX(const Instruction inst) noexcept
		{
			const Address address = indexedStore(inst);
			if (!checkAlignment<f32>(address, FaultAccess::Write) || !checkWritable(address, sizeof(f32)))
				return;
			write<u32>(address, getFloatBits(inst.fs()));
			advancePC();
		}
		// The displacement is measured from the instruction itself, like a branch's, so the address
		// is formed without materialising anything and without borrowing a register to hold it.
		forceinline Address pcRelative(const Instruction inst) const noexcept
		{
			return Address(static_cast<u32>(static_cast<i32>(_pc.value()) + inst.simm16()));
		}

		forceinline void LDRP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkAlignment<u32>(address))
				return;
			loaded(inst.rd(), read<u32>(address));
		}
		forceinline void LDRBP(const Instruction inst) noexcept { loaded(inst.rd(), read<u8>(pcRelative(inst))); }
		forceinline void LDRHP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkAlignment<u16>(address))
				return;
			loaded(inst.rd(), read<u16>(address));
		}
		forceinline void LDRSBP(const Instruction inst) noexcept { loaded(inst.rd(), static_cast<u32>(read<i8>(pcRelative(inst)))); }
		forceinline void LDRSHP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkAlignment<i16>(address))
				return;
			loaded(inst.rd(), static_cast<u32>(read<i16>(address)));
		}
		forceinline void FLDRP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkAlignment<f32>(address))
				return;
			loadedFloat(inst.fd(), read<u32>(address));
		}
		forceinline void STRP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkAlignment<u32>(address, FaultAccess::Write) || !checkWritable(address, sizeof(u32)))
				return;
			write<u32>(address, getReg(inst.rs()));
			advancePC();
		}
		forceinline void STRBP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkWritable(address, sizeof(u8)))
				return;
			write<u8>(address, static_cast<u8>(getReg(inst.rs())));
			advancePC();
		}
		forceinline void STRHP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkAlignment<u16>(address, FaultAccess::Write) || !checkWritable(address, sizeof(u16)))
				return;
			write<u16>(address, static_cast<u16>(getReg(inst.rs())));
			advancePC();
		}
		forceinline void FSTRP(const Instruction inst) noexcept
		{
			const Address address = pcRelative(inst);
			if (!checkAlignment<f32>(address, FaultAccess::Write) || !checkWritable(address, sizeof(f32)))
				return;
			write<u32>(address, getFloatBits(inst.fs()));
			advancePC();
		}
		forceinline void LEA(const Instruction inst) noexcept { setReg(inst.rd(), getReg(inst.rs()) + displacement(inst)); advancePC(); }

		// ---- Block memory: MCPY, MSET, MCMP, MSCAN ----------------------------------------------------
		//
		// One step does one chunk: up to a page, and never across a page boundary of any address it
		// touches, so each side is translated once and lies in one frame. Until rt reaches 0 the PC stays
		// on the instruction and the next step does the next chunk - an interrupt waits at most a page,
		// and a page fault leaves the registers saying how far the operation got, so the handler's IRET
		// resumes it. A chunk costs the CPU ceil(bytes / 8) cycles and the instruction 4 more when it ends
		// (plan/v2 SPEC 3.2), so a cycle budget still means something when one instruction can move a megabyte.

		static constexpr u32 BlockChunkBytes = Mmu::PageSize;

		static u32 blockChunk(u32 count, u32 a, u32 b) noexcept
		{
			u32 n = count < BlockChunkBytes ? count : BlockChunkBytes;
			const u32 toPageEndA = Mmu::PageSize - (a & (Mmu::PageSize - 1));
			const u32 toPageEndB = Mmu::PageSize - (b & (Mmu::PageSize - 1));
			n = n < toPageEndA ? n : toPageEndA;
			return n < toPageEndB ? n : toPageEndB;
		}

		// A chunk as one span of RAM or of VRAM, or nullptr when it does not lie wholly in the RAM a program reaches
		// or in the backed VRAM (the vector table and the BIOS, past the end of either): then it goes a byte at a
		// time through read<u8>/write<u8>, which say what every such byte does - and the loop stops at the first
		// that faults, leaving the registers as they were for the handler's IRET to run the chunk again.
		// A block instruction never reaches a device: a register is not a run of bytes (plan/v2 SPEC 5.1). Raises
		// MemoryFault and returns true when this side of the chunk is in the device window. A chunk never
		// crosses a page, and the window starts on one, so a chunk is wholly in it or wholly out.
		bool refuseDeviceBlock(Address address, Address physical, FaultAccess access, u32 size) noexcept
		{
			if (!MmioBus::contains(physical))
				return false;
			noteFault(address, access, size, FaultReason::MmioBlock);
			triggerInterrupt(InterruptNumber::MemoryFault);
			return true;
		}

		// `forWrite`: the chunk is stored to, so a VRAM chunk marks its pages as written.
		forceinline u8* blockSpan(Address physical, u32 size, bool forWrite) noexcept
		{
			const u64 base = physical.value();
			if (base >= Memory::UnrestrictedSegmentStartValue && base + size <= _memory.size()) [[likely]]
				return _memory.data() + base;
			return vramBlockSpan(physical.value(), size, forWrite);
		}

		neverinline u8* vramBlockSpan(u32 physical, u32 size, bool forWrite) noexcept
		{
			if (!_vram.backs(physical, size))
				return nullptr;
			const u32 offset = physical - Vram::BaseValue;
			return forWrite ? _vram.span(offset, size) : const_cast<u8*>(_vram.data()) + offset;
		}

		// A chunk a byte at a time, through read<u8>/write<u8>: false at the first byte that faults.
		neverinline bool copyBytewise(u32 dst, u32 src, u32 n) noexcept
		{
			for (u32 i = 0; i < n; ++i)
			{
				const u8 byte = read<u8>(Address(src + i));
				if (_faulted)
					return false;
				write<u8>(Address(dst + i), byte);
				if (_faulted)
					return false;
			}
			return true;
		}

		neverinline bool setBytewise(u32 dst, u8 value, u32 n) noexcept
		{
			for (u32 i = 0; i < n; ++i)
			{
				write<u8>(Address(dst + i), value);
				if (_faulted)
					return false;
			}
			return true;
		}

		void chargeBlock(u64 start, u32 bytes, bool finished) noexcept
		{
			_cycles = start + isa::cycles::ofBlockChunk(bytes) + (finished ? isa::cycles::BlockBase : 0);
		}

		void MCPY(const Instruction inst) noexcept
		{
			const u64 start = _cycles;
			const u32 count = getReg(inst.rt());
			if (count == 0)
			{
				_cycles += isa::cycles::BlockBase;
				advancePC();
				return;
			}
			const u32 dst = getReg(inst.rd());
			const u32 src = getReg(inst.rs());
			const u32 n = blockChunk(count, dst, src);
			if (_accessObserver) [[unlikely]]
			{
				_accessObserver(AccessKind::Read, src, n);
				_accessObserver(AccessKind::Write, dst, n);
			}
			if (!checkWritable(Address(dst), n))
				return;
			const auto from = translate(Address(src), MmuAccess::Read);
			if (!from.has_value())
				return;
			const auto to = translate(Address(dst), MmuAccess::Write);
			if (!to.has_value())
				return;
			if (refuseDeviceBlock(Address(src), *from, FaultAccess::Read, n) || refuseDeviceBlock(Address(dst), *to, FaultAccess::Write, n))
				return;
			u8* s = blockSpan(*from, n, false);
			u8* d = blockSpan(*to, n, true);
			if (s != nullptr && d != nullptr)
			{
				// Lowest byte first, as the instruction is defined: an overlap with the destination above the
				// source repeats the bytes it has just written, as a byte loop would.
				if (d > s && d < s + n)
				{
					for (u32 i = 0; i < n; ++i)
						d[i] = s[i];
				}
				else
					std::memmove(d, s, n);
			}
			else if (!copyBytewise(dst, src, n))
				return;
			setReg(inst.rd(), dst + n);
			setReg(inst.rs(), src + n);
			setReg(inst.rt(), count - n);
			chargeBlock(start, n, count == n);
			if (count == n)
				advancePC();
		}

		void MSET(const Instruction inst) noexcept
		{
			const u64 start = _cycles;
			const u32 count = getReg(inst.rt());
			if (count == 0)
			{
				_cycles += isa::cycles::BlockBase;
				advancePC();
				return;
			}
			const u32 dst = getReg(inst.rd());
			const u8 value = static_cast<u8>(getReg(inst.rs()));
			const u32 n = blockChunk(count, dst, dst);
			if (_accessObserver) [[unlikely]]
				_accessObserver(AccessKind::Write, dst, n);
			if (!checkWritable(Address(dst), n))
				return;
			const auto to = translate(Address(dst), MmuAccess::Write);
			if (!to.has_value())
				return;
			if (refuseDeviceBlock(Address(dst), *to, FaultAccess::Write, n))
				return;
			if (u8* d = blockSpan(*to, n, true); d != nullptr)
				std::memset(d, value, n);
			else if (!setBytewise(dst, value, n))
				return;
			setReg(inst.rd(), dst + n);
			setReg(inst.rt(), count - n);
			chargeBlock(start, n, count == n);
			if (count == n)
				advancePC();
		}

		// Z set: the blocks are equal (or the byte was not found) and rt is 0. Otherwise rd and rs are at the
		// first bytes that differ, rt counts them and what follows, and the flags are those of a CMP of the
		// two bytes as unsigned numbers: C and N set when [rd]'s is the lower.
		void MCMP(const Instruction inst) noexcept
		{
			const u64 start = _cycles;
			const u32 count = getReg(inst.rt());
			if (count == 0)
			{
				_cycles += isa::cycles::BlockBase;
				zero(true); carry(false); sign(false); overflow(false);
				advancePC();
				return;
			}
			const u32 a = getReg(inst.rd());
			const u32 b = getReg(inst.rs());
			const u32 n = blockChunk(count, a, b);
			if (_accessObserver) [[unlikely]]
			{
				_accessObserver(AccessKind::Read, a, n);
				_accessObserver(AccessKind::Read, b, n);
			}
			const auto pa = translate(Address(a), MmuAccess::Read);
			if (!pa.has_value())
				return;
			const auto pb = translate(Address(b), MmuAccess::Read);
			if (!pb.has_value())
				return;
			if (refuseDeviceBlock(Address(a), *pa, FaultAccess::Read, n) || refuseDeviceBlock(Address(b), *pb, FaultAccess::Read, n))
				return;
			const u8* x = blockSpan(*pa, n, false);
			const u8* y = blockSpan(*pb, n, false);
			u32 i = 0;
			u8 left = 0, right = 0;
			if (x != nullptr && y != nullptr)
			{
				while (i < n && x[i] == y[i])
					++i;
				if (i < n) { left = x[i]; right = y[i]; }
			}
			else
			{
				for (; i < n; ++i)
				{
					left = read<u8>(Address(a + i));
					right = read<u8>(Address(b + i));
					if (_faulted)
						return;
					if (left != right)
						break;
				}
			}
			if (i < n)
			{
				setReg(inst.rd(), a + i);
				setReg(inst.rs(), b + i);
				setReg(inst.rt(), count - i);
				zero(false);
				carry(left < right);
				sign(left < right);
				overflow(false);
				chargeBlock(start, i + 1, true);    // the differing byte was read too
				advancePC();
				return;
			}
			setReg(inst.rd(), a + n);
			setReg(inst.rs(), b + n);
			setReg(inst.rt(), count - n);
			chargeBlock(start, n, count == n);
			if (count == n)
			{
				zero(true); carry(false); sign(false); overflow(false);
				advancePC();
			}
		}

		// Z set: not found, rd past the block and rt 0. Otherwise rd is at the byte and rt counts it and
		// what follows.
		void MSCAN(const Instruction inst) noexcept
		{
			const u64 start = _cycles;
			const u32 count = getReg(inst.rt());
			if (count == 0)
			{
				_cycles += isa::cycles::BlockBase;
				zero(true);
				advancePC();
				return;
			}
			const u32 a = getReg(inst.rd());
			const u8 value = static_cast<u8>(getReg(inst.rs()));
			const u32 n = blockChunk(count, a, a);
			if (_accessObserver) [[unlikely]]
				_accessObserver(AccessKind::Read, a, n);
			const auto pa = translate(Address(a), MmuAccess::Read);
			if (!pa.has_value())
				return;
			if (refuseDeviceBlock(Address(a), *pa, FaultAccess::Read, n))
				return;
			u32 i = n;
			if (const u8* x = blockSpan(*pa, n, false); x != nullptr)
			{
				if (const void* hit = std::memchr(x, value, n); hit != nullptr)
					i = static_cast<u32>(static_cast<const u8*>(hit) - x);
			}
			else
			{
				for (u32 k = 0; k < n; ++k)
				{
					const u8 byte = read<u8>(Address(a + k));
					if (_faulted)
						return;
					if (byte == value)
					{
						i = k;
						break;
					}
				}
			}
			if (i < n)
			{
				setReg(inst.rd(), a + i);
				setReg(inst.rt(), count - i);
				zero(false);
				chargeBlock(start, i + 1, true);    // the byte found was read too
				advancePC();
				return;
			}
			setReg(inst.rd(), a + n);
			setReg(inst.rt(), count - n);
			chargeBlock(start, n, count == n);
			if (count == n)
			{
				zero(true);
				advancePC();
			}
		}

		forceinline void JP(const Instruction inst) noexcept { _pc += inst.simm24().signedValue(); }
		forceinline void JPR(const Instruction inst) noexcept { _pc = Address(getReg(inst.rs())); }
		forceinline void CMP(const Instruction inst) noexcept
		{
			const u32 a = getReg(inst.rs());
			const u32 b = getReg(inst.rt());

			zero(a == b);
			sign(((a - b) & 0x80000000u) != 0); // bit 31 of the wrapped difference; Overflow (below) says if it wrapped
			carry(a < b);
			overflow(((a ^ b) & (a ^ (a - b))) & 0x80000000);

			advancePC();
		}
		forceinline void CMPI(const Instruction inst) noexcept
		{
			const u32 a = getReg(inst.rs());
			const u32 b = static_cast<u32>(inst.simm16());

			zero(a == b);
			sign(((a - b) & 0x80000000u) != 0); // bit 31 of the wrapped difference; Overflow (below) says if it wrapped
			carry(a < b);
			overflow(((a ^ b) & (a ^ (a - b))) & 0x80000000);

			advancePC();
		}
		forceinline void FCMP(const Instruction inst) noexcept
		{
			const f32 a = getFloatReg(inst.fs());
			const f32 b = getFloatReg(inst.ft());

			zero(a == b);
			sign(std::signbit(a - b));
			carry(a < b);
			overflow(false); // Overflow doesn't really make sense for floating-point comparisons

			advancePC();
		}
		forceinline void JZ(const Instruction inst) noexcept { executeJumpIfFlag<ExecutionFlag::Zero>(inst); }
		forceinline void JZR(const Instruction inst) noexcept { executeJumpRegIfFlag<ExecutionFlag::Zero>(inst); }
		forceinline void JNZ(const Instruction inst) noexcept { executeJumpIfNotFlag<ExecutionFlag::Zero>(inst); }
		forceinline void JNZR(const Instruction inst) noexcept { executeJumpRegIfNotFlag<ExecutionFlag::Zero>(inst); }
		forceinline void JC(const Instruction inst) noexcept { executeJumpIfFlag<ExecutionFlag::Carry>(inst); }
		forceinline void JCR(const Instruction inst) noexcept { executeJumpRegIfFlag<ExecutionFlag::Carry>(inst); }
		forceinline void JNC(const Instruction inst) noexcept { executeJumpIfNotFlag<ExecutionFlag::Carry>(inst); }
		forceinline void JNCR(const Instruction inst) noexcept { executeJumpRegIfNotFlag<ExecutionFlag::Carry>(inst); }
		forceinline void JS(const Instruction inst) noexcept { executeJumpIfFlag<ExecutionFlag::Sign>(inst); }
		forceinline void JSR(const Instruction inst) noexcept { executeJumpRegIfFlag<ExecutionFlag::Sign>(inst); }
		forceinline void JNS(const Instruction inst) noexcept { executeJumpIfNotFlag<ExecutionFlag::Sign>(inst); }
		forceinline void JNSR(const Instruction inst) noexcept { executeJumpRegIfNotFlag<ExecutionFlag::Sign>(inst); }
		forceinline void JO(const Instruction inst) noexcept { executeJumpIfFlag<ExecutionFlag::Overflow>(inst); }
		forceinline void JOR(const Instruction inst) noexcept { executeJumpRegIfFlag<ExecutionFlag::Overflow>(inst); }
		forceinline void JNO(const Instruction inst) noexcept { executeJumpIfNotFlag<ExecutionFlag::Overflow>(inst); }
		forceinline void JNOR(const Instruction inst) noexcept { executeJumpRegIfNotFlag<ExecutionFlag::Overflow>(inst); }

		forceinline void JGR(const Instruction inst) noexcept { executeJumpIf(inst, !isEqual() && !signedLess()); }
		forceinline void JGRR(const Instruction inst) noexcept { executeJumpRegIf(inst, !isEqual() && !signedLess()); }
		forceinline void JGE(const Instruction inst) noexcept { executeJumpIf(inst, !signedLess()); }
		forceinline void JGER(const Instruction inst) noexcept { executeJumpRegIf(inst, !signedLess()); }
		forceinline void JLS(const Instruction inst) noexcept { executeJumpIf(inst, signedLess()); }
		forceinline void JLSR(const Instruction inst) noexcept { executeJumpRegIf(inst, signedLess()); }
		forceinline void JLE(const Instruction inst) noexcept { executeJumpIf(inst, isEqual() || signedLess()); }
		forceinline void JLER(const Instruction inst) noexcept { executeJumpRegIf(inst, isEqual() || signedLess()); }

		forceinline void JAB(const Instruction inst) noexcept { executeJumpIf(inst, !unsignedBelow() && !isEqual()); }
		forceinline void JABR(const Instruction inst) noexcept { executeJumpRegIf(inst, !unsignedBelow() && !isEqual()); }
		forceinline void JAE(const Instruction inst) noexcept { executeJumpIf(inst, !unsignedBelow()); }
		forceinline void JAER(const Instruction inst) noexcept { executeJumpRegIf(inst, !unsignedBelow()); }
		forceinline void JBL(const Instruction inst) noexcept { executeJumpIf(inst, unsignedBelow()); }
		forceinline void JBLR(const Instruction inst) noexcept { executeJumpRegIf(inst, unsignedBelow()); }
		forceinline void JBE(const Instruction inst) noexcept { executeJumpIf(inst, unsignedBelow() || isEqual()); }
		forceinline void JBER(const Instruction inst) noexcept { executeJumpRegIf(inst, unsignedBelow() || isEqual()); }
		forceinline void CALL(const Instruction inst) noexcept
		{
			if (!push<u32>((_pc + Instruction::SizeInBytes).value())) // Push return address onto the stack
				return;
			// signedValue() sign-extends; Address(i24) would zero-extend and send a backward call
			// roughly 16 MiB forward.
			_pc += inst.simm24().signedValue(); // Jump to target address
		}
		forceinline void CALLR(const Instruction inst) noexcept
		{
			if (!push<u32>((_pc + Instruction::SizeInBytes).value())) // Push return address onto the stack
				return;
			_pc = Address(getReg(inst.rs())); // Jump to target address
		}
		// The return address is written before the jump, so `bl r1, self` is a call to itself that
		// still comes back. Nothing is pushed, so nothing can overflow.
		forceinline void BL(const Instruction inst) noexcept
		{
			const u32 returnAddress = (_pc + Instruction::SizeInBytes).value();
			const i32 displacement = inst.simm20();
			setReg(inst.rd(), returnAddress);
			_pc += displacement;
		}
		forceinline void BLR(const Instruction inst) noexcept
		{
			const u32 returnAddress = (_pc + Instruction::SizeInBytes).value();
			const u32 target = getReg(inst.rs()); // read first: rd and rs may be the same register
			setReg(inst.rd(), returnAddress);
			_pc = Address(target);
		}
		forceinline void RET(const Instruction inst) noexcept
		{
			if (const auto target = pop<u32>())
				_pc = Address(*target);
		}

		forceinline void PUSH(const Instruction inst) noexcept { if (push<u32>(getReg(inst.rs()))) advancePC(); }
		forceinline void POP(const Instruction inst) noexcept { if (const auto v = pop<u32>()) { setReg(inst.rd(), *v); advancePC(); } }
		forceinline void PUSHF(const Instruction inst) noexcept { if (push<u32>(_flags.value())) advancePC(); }
		forceinline void POPF(const Instruction inst) noexcept { if (const auto v = pop<u32>()) { _flags = *v; advancePC(); } }
		// All or nothing. A half-saved frame is worse than one that never started: the epilogue's
		// POPM would restore the wrong registers out of the wrong slots and carry on as if it had
		// worked, so the room for the whole mask is checked before the first word goes down.
		forceinline void PUSHM(const Instruction inst) noexcept
		{
			const u16 mask = inst.imm16();
			const u32 words = static_cast<u32>(std::popcount(mask));
			if (!hasStackRoom(words * static_cast<u32>(sizeof(u32))))
			{
				triggerInterrupt(InterruptNumber::StackOverflow);
				return;
			}

			for (u32 i = GeneralPurposeRegisterPool::Count; i-- > 0;)
			{
				if (mask & (1u << i))
				{
					// The whole-mask room check above only rules out StackOverflow; a page fault is a
					// second, independent way a push here can fail, and this loop has to stop the
					// moment one does; a push already made this discipline to itself (see push<T>), but
					// nothing stopped this loop from calling it again for the next register in the
					// mask, onto memory that instruction was never actually supposed to touch.
					if (!push<u32>(getReg(static_cast<u8>(i))))
						break;
				}
			}
			advancePC();
		}
		forceinline void POPM(const Instruction inst) noexcept
		{
			const u16 mask = inst.imm16();
			const u32 words = static_cast<u32>(std::popcount(mask));
			if (!hasStackData(words * static_cast<u32>(sizeof(u32))))
			{
				triggerInterrupt(InterruptNumber::StackOverflow);
				return;
			}

			for (u32 i = 0; i < GeneralPurposeRegisterPool::Count; ++i)
			{
				if (mask & (1u << i))
				{
					const auto value = pop<u32>();
					if (!value.has_value())
						break; // Same reasoning as PUSHM's loop: stop at the first fault, don't paper over it.
					setReg(static_cast<u8>(i), *value);
				}
			}
			advancePC();
		}
		// The saved fp and the frame are one indivisible step: a prologue that pushed fp and then
		// found no room for the frame would leave a function running on a stack it does not own.
		forceinline void ENTER(const Instruction inst) noexcept
		{
			const u32 frameSize = inst.imm16();
			if (!hasStackRoom(static_cast<u32>(sizeof(u32)) + frameSize))
			{
				triggerInterrupt(InterruptNumber::StackOverflow);
				return;
			}

			(void)push<u32>(fp());
			fp(sp());
			sp(sp() - frameSize);
			advancePC();
		}
		forceinline void LEAVE(const Instruction inst) noexcept
		{
			sp(fp());
			if (const auto saved = pop<u32>())
			{
				fp(*saved);
				advancePC();
			}
		}
		forceinline void FPUSH(const Instruction inst) noexcept { if (push<u32>(getFloatBits(inst.fs()))) advancePC(); }
		forceinline void FPOP(const Instruction inst) noexcept { if (const auto v = pop<u32>()) { setFloatBits(inst.fd(), *v); advancePC(); } }
		// The float counterpart of PUSHM/POPM, over the float bank. Same all-or-nothing rule: the
		// room for the whole mask is checked before the first word goes down, and the store order
		// (highest set bit first) is the mirror of the load order (f0 first), so a matching pair
		// round-trips whatever the mask.
		forceinline void FPUSHM(const Instruction inst) noexcept
		{
			const u16 mask = inst.imm16();
			const u32 words = static_cast<u32>(std::popcount(mask));
			if (!hasStackRoom(words * static_cast<u32>(sizeof(u32))))
			{
				triggerInterrupt(InterruptNumber::StackOverflow);
				return;
			}

			for (u32 i = FloatingPointRegisterPool::Count; i-- > 0;)
			{
				if (mask & (1u << i))
				{
					if (!push<u32>(getFloatBits(static_cast<u8>(i))))
						break;
				}
			}
			advancePC();
		}
		forceinline void FPOPM(const Instruction inst) noexcept
		{
			const u16 mask = inst.imm16();
			const u32 words = static_cast<u32>(std::popcount(mask));
			if (!hasStackData(words * static_cast<u32>(sizeof(u32))))
			{
				triggerInterrupt(InterruptNumber::StackOverflow);
				return;
			}

			for (u32 i = 0; i < FloatingPointRegisterPool::Count; ++i)
			{
				if (mask & (1u << i))
				{
					const auto value = pop<u32>();
					if (!value.has_value())
						break;
					setFloatBits(static_cast<u8>(i), *value);
				}
			}
			advancePC();
		}

		forceinline void ITOF(const Instruction inst) noexcept { setFloatReg(inst.fd(), static_cast<f32>(getReg(inst.rs()))); advancePC(); }
		forceinline void IITOF(const Instruction inst) noexcept { setFloatReg(inst.fd(), static_cast<f32>(static_cast<i32>(getReg(inst.rs())))); advancePC(); }
		forceinline void FTOI(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(getFloatReg(inst.fs()))); advancePC(); }
		forceinline void FTOII(const Instruction inst) noexcept { setReg(inst.rd(), static_cast<u32>(static_cast<i32>(getFloatReg(inst.fs())))); advancePC(); }
		forceinline void MTF(const Instruction inst) noexcept { _fregisters[inst.fd()].setBitsFromRegister(_registers[inst.rs()]); advancePC(); }
		forceinline void MFF(const Instruction inst) noexcept { _registers.set(inst.rd(), _fregisters[inst.fs()].bitsAsRegister()); advancePC(); }

		// An instruction the machine does not run (plan/v2 SPEC 5.4): IllegalInstruction at the word itself, and why in
		// SystemControl's FaultReason.
		forceinline void illegal(FaultReason reason) noexcept
		{
			noteFault(_pc, FaultAccess::Execute, static_cast<u32>(Instruction::Size), reason);
			triggerInterrupt(InterruptNumber::IllegalInstruction);
		}

		// Whether a 64-bit instruction's pair fields and subfield are ones the machine runs (wide.h); raises the fault
		// and returns false when they are not. Asked by the 64-bit handlers alone, so no other instruction pays for it.
		forceinline bool decoded(const Instruction inst) noexcept
		{
			switch (wide::check(inst))
			{
				case wide::Decode::Ok: return true;
				case wide::Decode::RegisterPair: illegal(FaultReason::RegisterPair); return false;
				case wide::Decode::BadSubfield: illegal(FaultReason::BadSubfield); return false;
			}
			return false;
		}

		// ---- 64-bit integers on register pairs (plan/v2 SPEC 6) ---------------------------------------------------------
		//
		// A pair field holds its even register, which decoded() has checked: the low word is there and the high word in
		// the next one, as in memory. The flags are SPEC 6.3's.

		forceinline u64 getPair(u8 field) const noexcept
		{
			return static_cast<u64>(getReg(field)) | (static_cast<u64>(getReg(static_cast<u8>(field + 1))) << 32);
		}
		forceinline void setPair(u8 field, u64 value) noexcept
		{
			setReg(field, static_cast<u32>(value));
			setReg(static_cast<u8>(field + 1), static_cast<u32>(value >> 32));
		}

		// Zero and Sign of a 64-bit result, Carry and Overflow as given.
		forceinline void flags64(u64 result, bool carried, bool overflowed) noexcept
		{
			zero(result == 0);
			sign((result >> 63) != 0);
			carry(carried);
			overflow(overflowed);
		}

		forceinline void executeResult64(u8 dest, u64 result) noexcept
		{
			flags64(result, false, false);
			setPair(dest, result);
			advancePC();
		}

		forceinline void executeAdd64(u8 dest, u64 a, u64 b) noexcept
		{
			const u64 result = a + b;
			flags64(result, result < a, ((~(a ^ b) & (a ^ result)) >> 63) != 0);
			setPair(dest, result);
			advancePC();
		}

		// sub64, neg64 (0 - xs) and cmp64, which keeps only the flags.
		forceinline void executeSub64(u8 dest, u64 a, u64 b, bool keep = true) noexcept
		{
			const u64 result = a - b;
			flags64(result, a < b, (((a ^ b) & (a ^ result)) >> 63) != 0);
			if (keep)
				setPair(dest, result);
			advancePC();
		}

		// A shift by the low six bits of `amount`. By 0 it changes nothing, flags included, as the 32-bit shifts do; otherwise
		// Carry is the last bit shifted out.
		forceinline void executeShift64(u8 dest, u64 value, u32 amount, wide::ShiftKind kind) noexcept
		{
			amount &= 63u;
			if (amount == 0)
			{
				setPair(dest, value);
				advancePC();
				return;
			}
			u64 result = 0;
			bool out = false;
			switch (kind)
			{
				case wide::ShiftKind::Shl:
					result = value << amount;
					out = ((value >> (64u - amount)) & 1u) != 0;
					break;
				case wide::ShiftKind::Shr:
					result = value >> amount;
					out = ((value >> (amount - 1u)) & 1u) != 0;
					break;
				default:
					result = static_cast<u64>(static_cast<i64>(value) >> amount);
					out = ((static_cast<i64>(value) >> (amount - 1u)) & 1) != 0;
					break;
			}
			flags64(result, out, false);
			setPair(dest, result);
			advancePC();
		}

		// A 64-bit load (SPEC 6.4): aligned to 4, low word first. Memory is one access and pays for one; a device is two 32-bit
		// accesses, low then high, each paying its own - which is what lets ldrd read a latched pair like the timer's
		// CyclesLow/CyclesHigh. Nothing is written until both words are in, so a fault on the second leaves the pair alone.
		// A 64-bit access sits on a 4-byte boundary (SPEC 6.4); a fault says it was 8 bytes wide.
		forceinline bool checkAlignment64(Address address, FaultAccess access) noexcept
		{
			if ((address.value() & 3u) == 0)
				return true;
			noteFault(address, access, static_cast<u32>(sizeof(u64)), reachesDevice(address) ? FaultReason::MmioWidth : FaultReason::Alignment);
			triggerInterrupt(InterruptNumber::AlignmentFault);
			return false;
		}

		// The 64 bits at `address`, or empty when the access faulted (and the fault has been raised).
		forceinline std::optional<u64> read64(Address address) noexcept
		{
			if (!checkAlignment64(address, FaultAccess::Read))
				return std::nullopt;
			const u32 low = read<u32>(address);
			if (_faulted)
				return std::nullopt;
			const u32 high = read<u32>(Address(address.value() + 4u), false);
			if (_faulted)
				return std::nullopt;
			return static_cast<u64>(low) | (static_cast<u64>(high) << 32);
		}

		forceinline void load64(u8 dest, Address address) noexcept
		{
			if (const auto value = read64(address))
			{
				setPair(dest, *value);
				advancePC();
			}
		}

		// A 64-bit store, the same way round. A fault on the second word leaves the first written; the handler's IRET runs
		// the whole store again, which writes the same first word.
		// Writes 64 bits at `address`; false when the access faulted (and the fault has been raised).
		forceinline bool write64(Address address, u64 value) noexcept
		{
			if (!checkAlignment64(address, FaultAccess::Write) || !checkWritable(address, sizeof(u64)))
				return false;
			write<u32>(address, static_cast<u32>(value));
			if (_faulted)
				return false;
			write<u32>(Address(address.value() + 4u), static_cast<u32>(value >> 32), false);
			return !_faulted;
		}

		forceinline void store64(Address address, u64 value) noexcept
		{
			if (write64(address, value))
				advancePC();
		}

		forceinline void ADD64(const Instruction inst) noexcept { if (decoded(inst)) executeAdd64(inst.rd(), getPair(inst.rs()), getPair(inst.rt())); }
		forceinline void SUB64(const Instruction inst) noexcept { if (decoded(inst)) executeSub64(inst.rd(), getPair(inst.rs()), getPair(inst.rt())); }
		forceinline void NEG64(const Instruction inst) noexcept { if (decoded(inst)) executeSub64(inst.rd(), 0, getPair(inst.rs())); }
		forceinline void CMP64(const Instruction inst) noexcept { if (decoded(inst)) executeSub64(0, getPair(inst.rs()), getPair(inst.rt()), false); }
		// The products: Zero and Sign of the 64-bit result, Carry and Overflow clear (SPEC 6.3).
		forceinline void MULL(const Instruction inst) noexcept
		{
			if (decoded(inst))
				executeResult64(inst.rd(), static_cast<u64>(getReg(inst.rs())) * static_cast<u64>(getReg(inst.rt())));
		}
		forceinline void IMULL(const Instruction inst) noexcept
		{
			if (decoded(inst))
				executeResult64(inst.rd(), static_cast<u64>(static_cast<i64>(static_cast<i32>(getReg(inst.rs()))) * static_cast<i64>(static_cast<i32>(getReg(inst.rt())))));
		}
		forceinline void MUL64(const Instruction inst) noexcept { if (decoded(inst)) executeResult64(inst.rd(), getPair(inst.rs()) * getPair(inst.rt())); }
		// The divisions trap on a zero divisor the way div does, and leave the destination alone. INT64_MIN / -1 does not
		// fit: it gives INT64_MIN and sets Overflow, and its remainder is 0.
		forceinline void DIV64(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			const u64 divisor = getPair(inst.rt());
			if (divisor == 0)
			{
				divisionByZero();
				return;
			}
			executeResult64(inst.rd(), getPair(inst.rs()) / divisor);
		}
		forceinline void MOD64(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			const u64 divisor = getPair(inst.rt());
			if (divisor == 0)
			{
				divisionByZero();
				return;
			}
			executeResult64(inst.rd(), getPair(inst.rs()) % divisor);
		}
		forceinline void IDIV64(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			const i64 dividend = static_cast<i64>(getPair(inst.rs()));
			const i64 divisor = static_cast<i64>(getPair(inst.rt()));
			if (divisor == 0)
			{
				divisionByZero();
				return;
			}
			const bool overflowed = dividend == std::numeric_limits<i64>::min() && divisor == -1;
			const u64 result = overflowed ? static_cast<u64>(dividend) : static_cast<u64>(dividend / divisor);
			flags64(result, false, overflowed);
			setPair(inst.rd(), result);
			advancePC();
		}
		forceinline void IMOD64(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			const i64 dividend = static_cast<i64>(getPair(inst.rs()));
			const i64 divisor = static_cast<i64>(getPair(inst.rt()));
			if (divisor == 0)
			{
				divisionByZero();
				return;
			}
			executeResult64(inst.rd(), divisor == -1 ? 0 : static_cast<u64>(dividend % divisor));
		}
		forceinline void SHL64(const Instruction inst) noexcept { if (decoded(inst)) executeShift64(inst.rd(), getPair(inst.rs()), getReg(inst.rt()), wide::ShiftKind::Shl); }
		forceinline void SHR64(const Instruction inst) noexcept { if (decoded(inst)) executeShift64(inst.rd(), getPair(inst.rs()), getReg(inst.rt()), wide::ShiftKind::Shr); }
		forceinline void SAR64(const Instruction inst) noexcept { if (decoded(inst)) executeShift64(inst.rd(), getPair(inst.rs()), getReg(inst.rt()), wide::ShiftKind::Sar); }
		forceinline void SHI64(const Instruction inst) noexcept
		{
			if (decoded(inst))
				executeShift64(inst.rd(), getPair(inst.rs()), fields::ShiftAmount::get(inst.raw()), static_cast<wide::ShiftKind>(fields::ShiftKind::get(inst.raw())));
		}
		forceinline void SXT64(const Instruction inst) noexcept
		{
			if (decoded(inst))
				executeResult64(inst.rd(), static_cast<u64>(static_cast<i64>(static_cast<i32>(getReg(inst.rs())))));
		}
		// clz64/ctz64/popcnt64 into a 32-bit register, with the flags the 32-bit ones set.
		forceinline void BITS64(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			const u64 value = getPair(inst.rs());
			switch (static_cast<wide::BitOp>(fields::BitOp::get(inst.raw())))
			{
				case wide::BitOp::Clz: executeResult(inst.rd(), static_cast<u32>(std::countl_zero(value))); break;
				case wide::BitOp::Ctz: executeResult(inst.rd(), static_cast<u32>(std::countr_zero(value))); break;
				default:               executeResult(inst.rd(), static_cast<u32>(std::popcount(value))); break;
			}
		}
		forceinline void MOV64(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			setPair(inst.rd(), getPair(inst.rs()));
			advancePC();
		}
		forceinline void LDRD(const Instruction inst) noexcept { if (decoded(inst)) load64(inst.rd(), Address(getReg(inst.rs()) + displacement(inst))); }
		forceinline void LDRDX(const Instruction inst) noexcept { if (decoded(inst)) load64(inst.rd(), indexed(inst)); }
		forceinline void LDRDP(const Instruction inst) noexcept { if (decoded(inst)) load64(inst.rd(), pcRelative(inst)); }
		forceinline void STRD(const Instruction inst) noexcept { if (decoded(inst)) store64(Address(getReg(inst.rd()) + displacement(inst)), getPair(inst.rs())); }
		forceinline void STRDX(const Instruction inst) noexcept { if (decoded(inst)) store64(indexedStore(inst), getPair(inst.rs())); }

		// ---- binary64 doubles on float register pairs (plan/v2 SPEC 6) --------------------------------------------------
		//
		// dN is f(2N):f(2N+1), low word in the even register. The arithmetic is the host's double, rounded to nearest
		// even; the flags are those of the 32-bit instruction each one mirrors.

		forceinline u64 getDoubleBits(u8 field) const noexcept
		{
			return static_cast<u64>(getFloatBits(field)) | (static_cast<u64>(getFloatBits(static_cast<u8>(field + 1))) << 32);
		}
		forceinline void setDoubleBits(u8 field, u64 bits) noexcept
		{
			setFloatBits(field, static_cast<u32>(bits));
			setFloatBits(static_cast<u8>(field + 1), static_cast<u32>(bits >> 32));
		}
		forceinline f64 getDouble(u8 field) const noexcept { return std::bit_cast<f64>(getDoubleBits(field)); }
		forceinline void setDouble(u8 field, f64 value) noexcept { setDoubleBits(field, std::bit_cast<u64>(value)); }

		// fadd's flags: Zero and Sign of the result, Carry clear, Overflow when finite operands gave an infinity.
		forceinline void executeDoubleArithmetic(u8 dest, f64 result, f64 a, f64 b) noexcept
		{
			const auto resultClass = std::fpclassify(result);
			zero(resultClass == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(resultClass == FP_INFINITE && std::fpclassify(a) != FP_INFINITE && std::fpclassify(b) != FP_INFINITE);
			setDouble(dest, result);
			advancePC();
		}

		// A double to an integer (fcvt, SPEC 6.5): truncated toward zero and saturated at the ends of the range; NaN is 0.
		template <typename Int>
		static Int saturate(f64 value) noexcept
		{
			if (std::isnan(value))
				return 0;
			constexpr f64 Low = static_cast<f64>(std::numeric_limits<Int>::min());
			constexpr f64 High = static_cast<f64>(std::numeric_limits<Int>::max());   // for 64 bits, rounds up to 2^63 / 2^64
			const f64 truncated = std::trunc(value);
			if (truncated <= Low)
				return std::numeric_limits<Int>::min();
			if (truncated >= High)
				return std::numeric_limits<Int>::max();
			return static_cast<Int>(truncated);
		}

		forceinline void FADDD(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			const f64 a = getDouble(inst.rs()), b = getDouble(inst.rt());
			executeDoubleArithmetic(inst.rd(), a + b, a, b);
		}
		forceinline void FSUBD(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			const f64 a = getDouble(inst.rs()), b = getDouble(inst.rt());
			executeDoubleArithmetic(inst.rd(), a - b, a, b);
		}
		forceinline void FMULD(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			const f64 a = getDouble(inst.rs()), b = getDouble(inst.rt());
			executeDoubleArithmetic(inst.rd(), a * b, a, b);
		}
		// A zero divisor traps as fdiv does, unless FeatureIeeeDivide asks for IEEE's infinity or NaN.
		forceinline void FDIVD(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			const f64 a = getDouble(inst.rs()), b = getDouble(inst.rt());
			if (b == 0.0 && !_ieeeDivide)
			{
				divisionByZero();
				return;
			}
			executeDoubleArithmetic(inst.rd(), a / b, a, b);
		}
		// One rounding, as SPEC 6.4 says: dd + ds * dt exactly, then rounded.
		forceinline void FMAD(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			const f64 accumulator = getDouble(inst.rd()), a = getDouble(inst.rs()), b = getDouble(inst.rt());
			executeDoubleArithmetic(inst.rd(), std::fma(a, b, accumulator), accumulator, a * b);
		}
		forceinline void FSQRTD(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			setDouble(inst.rd(), squareRoot(getDouble(inst.rs())));
			advancePC();
		}
		forceinline void FCMPD(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			const f64 a = getDouble(inst.rs()), b = getDouble(inst.rt());
			zero(a == b);
			sign(std::signbit(a - b));
			carry(a < b);
			overflow(false);
			advancePC();
		}
		forceinline void FMINMAXD(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			const f64 a = getDouble(inst.rs()), b = getDouble(inst.rt());
			const f64 result = fields::MinMax::get(inst.raw()) == 0 ? std::fmin(a, b) : std::fmax(a, b);
			zero(std::fpclassify(result) == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(false);
			setDouble(inst.rd(), result);
			advancePC();
		}
		// fmod's rule: a zero divisor traps unless FeatureIeeeDivide, and then the result is NaN.
		forceinline void FMODD(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			const f64 a = getDouble(inst.rs()), b = getDouble(inst.rt());
			if (b == 0.0 && !_ieeeDivide)
			{
				divisionByZero();
				return;
			}
			const f64 result = std::fmod(a, b);
			zero(std::fpclassify(result) == FP_ZERO);
			sign(std::signbit(result));
			carry(false);
			overflow(false);
			setDouble(inst.rd(), result);
			advancePC();
		}
		// fneg.d sets fneg's flags; the rest, like their 32-bit ones, set none.
		forceinline void FUNARYD(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			const f64 value = getDouble(inst.rs());
			switch (static_cast<wide::UnaryOp>(fields::UnaryOp::get(inst.raw())))
			{
				case wide::UnaryOp::Neg:
				{
					const f64 result = -value;
					const auto resultClass = std::fpclassify(result);
					zero(resultClass == FP_ZERO);
					sign(std::signbit(result));
					carry(false);
					overflow(false);
					setDouble(inst.rd(), result);
					break;
				}
				case wide::UnaryOp::Abs:   setDouble(inst.rd(), std::fabs(value)); break;
				case wide::UnaryOp::Round: setDouble(inst.rd(), std::nearbyint(value)); break;
				case wide::UnaryOp::Floor: setDouble(inst.rd(), std::floor(value)); break;
				case wide::UnaryOp::Ceil:  setDouble(inst.rd(), std::ceil(value)); break;
				default:                   setDouble(inst.rd(), std::trunc(value)); break;
			}
			advancePC();
		}
		forceinline void FCOPYSIGND(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			setDouble(inst.rd(), std::copysign(getDouble(inst.rs()), getDouble(inst.rt())));
			advancePC();
		}
		forceinline void FCLASSD(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			setReg(inst.rd(), classifyFloat(getDouble(inst.rs())));
			advancePC();
		}
		// The conversions of SPEC 6.5. To a float: rounded to nearest even. To an integer: saturate(). The ones that read
		// or write a 64-bit integer cost a cycle more.
		forceinline void FCVT(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			const u8 d = inst.rd();
			const u8 s = inst.rs();
			const auto kind = static_cast<wide::FcvtKind>(fields::FcvtKind::get(inst.raw()));
			switch (kind)
			{
				using enum wide::FcvtKind;
				case DS:  setDouble(d, static_cast<f64>(getFloatReg(s))); break;
				case SD:  setFloatReg(d, static_cast<f32>(getDouble(s))); break;
				case DW:  setDouble(d, static_cast<f64>(static_cast<i32>(getReg(s)))); break;
				case DWU: setDouble(d, static_cast<f64>(getReg(s))); break;
				case WD:  setReg(d, static_cast<u32>(saturate<i32>(getDouble(s)))); break;
				case WUD: setReg(d, saturate<u32>(getDouble(s))); break;
				case DL:  setDouble(d, static_cast<f64>(static_cast<i64>(getPair(s)))); break;
				case DLU: setDouble(d, static_cast<f64>(getPair(s))); break;
				case LD:  setPair(d, static_cast<u64>(saturate<i64>(getDouble(s)))); break;
				case LUD: setPair(d, saturate<u64>(getDouble(s))); break;
				case SL:  setFloatReg(d, static_cast<f32>(static_cast<i64>(getPair(s)))); break;
				case SLU: setFloatReg(d, static_cast<f32>(getPair(s))); break;
				case LS:  setPair(d, static_cast<u64>(saturate<i64>(static_cast<f64>(getFloatReg(s))))); break;
				default:  setPair(d, saturate<u64>(static_cast<f64>(getFloatReg(s)))); break;   // LUS
			}
			if (static_cast<u8>(kind) >= static_cast<u8>(wide::FcvtKind::DL))
				_cycles += 1;
			advancePC();
		}
		forceinline void FMOVD(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			setDoubleBits(inst.rd(), getDoubleBits(inst.rs()));
			advancePC();
		}
		forceinline void loadDouble(u8 dest, Address address) noexcept
		{
			if (const auto value = read64(address))
			{
				setDoubleBits(dest, *value);
				advancePC();
			}
		}
		forceinline void FLDRD(const Instruction inst) noexcept { if (decoded(inst)) loadDouble(inst.rd(), Address(getReg(inst.rs()) + displacement(inst))); }
		forceinline void FLDRDX(const Instruction inst) noexcept { if (decoded(inst)) loadDouble(inst.rd(), indexed(inst)); }
		forceinline void FLDRDP(const Instruction inst) noexcept { if (decoded(inst)) loadDouble(inst.rd(), pcRelative(inst)); }
		forceinline void FSTRD(const Instruction inst) noexcept { if (decoded(inst)) store64(Address(getReg(inst.rd()) + displacement(inst)), getDoubleBits(inst.rs())); }
		forceinline void FSTRDX(const Instruction inst) noexcept { if (decoded(inst)) store64(indexedStore(inst), getDoubleBits(inst.rs())); }
		// The bits as they are, between the two banks.
		forceinline void MTFD(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			setDoubleBits(inst.rd(), getPair(inst.rs()));
			advancePC();
		}
		forceinline void MFFD(const Instruction inst) noexcept
		{
			if (!decoded(inst))
				return;
			setPair(inst.rd(), getDoubleBits(inst.rs()));
			advancePC();
		}

		forceinline void INVALID(const Instruction inst) noexcept { illegal(FaultReason::UnknownOpcode); }

	private:
		using InstructionHandler = void (ExecutionEngine::*)(const Instruction) noexcept;
		static inline constexpr std::array<InstructionHandler, 256> InstructionHandlers = []() consteval noexcept -> std::array<InstructionHandler, 256>
		{
				// Aggregate initialisation with a single element only assigns index 0; every other
				// slot would stay null and calling one is a crash, not an illegal-instruction trap.
				std::array<InstructionHandler, 256> handlers{};
				handlers.fill(&ExecutionEngine::INVALID);

				// Control
				handlers[static_cast<u8>(Opcode::NOP)] = &ExecutionEngine::NOP;
				handlers[static_cast<u8>(Opcode::HALT)] = &ExecutionEngine::HALT;
				handlers[static_cast<u8>(Opcode::TRAP)] = &ExecutionEngine::TRAP;
				handlers[static_cast<u8>(Opcode::RESET)] = &ExecutionEngine::RESET;
				handlers[static_cast<u8>(Opcode::INT)] = &ExecutionEngine::INT;
				handlers[static_cast<u8>(Opcode::IRET)] = &ExecutionEngine::IRET;
				handlers[static_cast<u8>(Opcode::CLI)] = &ExecutionEngine::CLI;
				handlers[static_cast<u8>(Opcode::STI)] = &ExecutionEngine::STI;

				// MMU
				handlers[static_cast<u8>(Opcode::MTP)] = &ExecutionEngine::MTP;
				handlers[static_cast<u8>(Opcode::MFP)] = &ExecutionEngine::MFP;
				handlers[static_cast<u8>(Opcode::PGON)] = &ExecutionEngine::PGON;
				handlers[static_cast<u8>(Opcode::PGOFF)] = &ExecutionEngine::PGOFF;
				handlers[static_cast<u8>(Opcode::INVLPG)] = &ExecutionEngine::INVLPG;
				handlers[static_cast<u8>(Opcode::FLPG)] = &ExecutionEngine::FLPG;
				handlers[static_cast<u8>(Opcode::MFPF)] = &ExecutionEngine::MFPF;

				// Arithmetic
				handlers[static_cast<u8>(Opcode::ADD)] = &ExecutionEngine::ADD;
				handlers[static_cast<u8>(Opcode::ADDI)] = &ExecutionEngine::ADDI;
				handlers[static_cast<u8>(Opcode::ADDC)] = &ExecutionEngine::ADDC;
				handlers[static_cast<u8>(Opcode::ADDCI)] = &ExecutionEngine::ADDCI;
				handlers[static_cast<u8>(Opcode::FADD)] = &ExecutionEngine::FADD;
				handlers[static_cast<u8>(Opcode::SUB)] = &ExecutionEngine::SUB;
				handlers[static_cast<u8>(Opcode::SUBI)] = &ExecutionEngine::SUBI;
				handlers[static_cast<u8>(Opcode::SUBC)] = &ExecutionEngine::SUBC;
				handlers[static_cast<u8>(Opcode::SUBCI)] = &ExecutionEngine::SUBCI;
				handlers[static_cast<u8>(Opcode::FSUB)] = &ExecutionEngine::FSUB;
				handlers[static_cast<u8>(Opcode::MUL)] = &ExecutionEngine::MUL;
				handlers[static_cast<u8>(Opcode::MULI)] = &ExecutionEngine::MULI;
				handlers[static_cast<u8>(Opcode::IMUL)] = &ExecutionEngine::IMUL;
				handlers[static_cast<u8>(Opcode::IMULI)] = &ExecutionEngine::IMULI;
				handlers[static_cast<u8>(Opcode::FMUL)] = &ExecutionEngine::FMUL;
				handlers[static_cast<u8>(Opcode::DIV)] = &ExecutionEngine::DIV;
				handlers[static_cast<u8>(Opcode::DIVI)] = &ExecutionEngine::DIVI;
				handlers[static_cast<u8>(Opcode::IDIV)] = &ExecutionEngine::IDIV;
				handlers[static_cast<u8>(Opcode::IDIVI)] = &ExecutionEngine::IDIVI;
				handlers[static_cast<u8>(Opcode::FDIV)] = &ExecutionEngine::FDIV;
				handlers[static_cast<u8>(Opcode::MOD)] = &ExecutionEngine::MOD;
				handlers[static_cast<u8>(Opcode::MODI)] = &ExecutionEngine::MODI;
				handlers[static_cast<u8>(Opcode::IMOD)] = &ExecutionEngine::IMOD;
				handlers[static_cast<u8>(Opcode::IMODI)] = &ExecutionEngine::IMODI;
				handlers[static_cast<u8>(Opcode::FMOD)] = &ExecutionEngine::FMOD;
				handlers[static_cast<u8>(Opcode::FNEG)] = &ExecutionEngine::FNEG;

				// Logical
				handlers[static_cast<u8>(Opcode::AND)] = &ExecutionEngine::AND;
				handlers[static_cast<u8>(Opcode::ANDI)] = &ExecutionEngine::ANDI;
				handlers[static_cast<u8>(Opcode::OR)] = &ExecutionEngine::OR;
				handlers[static_cast<u8>(Opcode::ORI)] = &ExecutionEngine::ORI;
				handlers[static_cast<u8>(Opcode::XOR)] = &ExecutionEngine::XOR;
				handlers[static_cast<u8>(Opcode::XORI)] = &ExecutionEngine::XORI;
				handlers[static_cast<u8>(Opcode::NOT)] = &ExecutionEngine::NOT;

				// Shifts
				handlers[static_cast<u8>(Opcode::SHL)] = &ExecutionEngine::SHL;
				handlers[static_cast<u8>(Opcode::SHLI)] = &ExecutionEngine::SHLI;
				handlers[static_cast<u8>(Opcode::SHR)] = &ExecutionEngine::SHR;
				handlers[static_cast<u8>(Opcode::SHRI)] = &ExecutionEngine::SHRI;
				handlers[static_cast<u8>(Opcode::SAR)] = &ExecutionEngine::SAR;
				handlers[static_cast<u8>(Opcode::SARI)] = &ExecutionEngine::SARI;

				// Moves / Loads / Stores
				handlers[static_cast<u8>(Opcode::MOV)] = &ExecutionEngine::MOV;
				handlers[static_cast<u8>(Opcode::FMOV)] = &ExecutionEngine::FMOV;
				handlers[static_cast<u8>(Opcode::LI)] = &ExecutionEngine::LI;
				handlers[static_cast<u8>(Opcode::LUI)] = &ExecutionEngine::LUI;
				handlers[static_cast<u8>(Opcode::LDR)] = &ExecutionEngine::LDR;
				handlers[static_cast<u8>(Opcode::LDRB)] = &ExecutionEngine::LDRB;
				handlers[static_cast<u8>(Opcode::LDRH)] = &ExecutionEngine::LDRH;
				handlers[static_cast<u8>(Opcode::LDRSB)] = &ExecutionEngine::LDRSB;
				handlers[static_cast<u8>(Opcode::LDRSH)] = &ExecutionEngine::LDRSH;
				handlers[static_cast<u8>(Opcode::FLDR)] = &ExecutionEngine::FLDR;
				handlers[static_cast<u8>(Opcode::STR)] = &ExecutionEngine::STR;
				handlers[static_cast<u8>(Opcode::STRB)] = &ExecutionEngine::STRB;
				handlers[static_cast<u8>(Opcode::STRH)] = &ExecutionEngine::STRH;
				handlers[static_cast<u8>(Opcode::FSTR)] = &ExecutionEngine::FSTR;
				handlers[static_cast<u8>(Opcode::LEA)] = &ExecutionEngine::LEA;

				// Jumps / Branches / Calls
				handlers[static_cast<u8>(Opcode::JP)] = &ExecutionEngine::JP;
				handlers[static_cast<u8>(Opcode::JPR)] = &ExecutionEngine::JPR;
				handlers[static_cast<u8>(Opcode::CMP)] = &ExecutionEngine::CMP;
				handlers[static_cast<u8>(Opcode::CMPI)] = &ExecutionEngine::CMPI;
				handlers[static_cast<u8>(Opcode::FCMP)] = &ExecutionEngine::FCMP;
				handlers[static_cast<u8>(Opcode::JZ)] = &ExecutionEngine::JZ;
				handlers[static_cast<u8>(Opcode::JZR)] = &ExecutionEngine::JZR;
				handlers[static_cast<u8>(Opcode::JNZ)] = &ExecutionEngine::JNZ;
				handlers[static_cast<u8>(Opcode::JNZR)] = &ExecutionEngine::JNZR;
				handlers[static_cast<u8>(Opcode::JC)] = &ExecutionEngine::JC;
				handlers[static_cast<u8>(Opcode::JCR)] = &ExecutionEngine::JCR;
				handlers[static_cast<u8>(Opcode::JNC)] = &ExecutionEngine::JNC;
				handlers[static_cast<u8>(Opcode::JNCR)] = &ExecutionEngine::JNCR;
				handlers[static_cast<u8>(Opcode::JS)] = &ExecutionEngine::JS;
				handlers[static_cast<u8>(Opcode::JSR)] = &ExecutionEngine::JSR;
				handlers[static_cast<u8>(Opcode::JNS)] = &ExecutionEngine::JNS;
				handlers[static_cast<u8>(Opcode::JNSR)] = &ExecutionEngine::JNSR;
				handlers[static_cast<u8>(Opcode::CALL)] = &ExecutionEngine::CALL;
				handlers[static_cast<u8>(Opcode::CALLR)] = &ExecutionEngine::CALLR;
				handlers[static_cast<u8>(Opcode::BL)] = &ExecutionEngine::BL;
				handlers[static_cast<u8>(Opcode::BLR)] = &ExecutionEngine::BLR;
				handlers[static_cast<u8>(Opcode::RET)] = &ExecutionEngine::RET;
				handlers[static_cast<u8>(Opcode::JO)] = &ExecutionEngine::JO;
				handlers[static_cast<u8>(Opcode::JOR)] = &ExecutionEngine::JOR;
				handlers[static_cast<u8>(Opcode::JNO)] = &ExecutionEngine::JNO;
				handlers[static_cast<u8>(Opcode::JNOR)] = &ExecutionEngine::JNOR;
				handlers[static_cast<u8>(Opcode::JGR)] = &ExecutionEngine::JGR;
				handlers[static_cast<u8>(Opcode::JGRR)] = &ExecutionEngine::JGRR;
				handlers[static_cast<u8>(Opcode::JGE)] = &ExecutionEngine::JGE;
				handlers[static_cast<u8>(Opcode::JGER)] = &ExecutionEngine::JGER;
				handlers[static_cast<u8>(Opcode::JLS)] = &ExecutionEngine::JLS;
				handlers[static_cast<u8>(Opcode::JLSR)] = &ExecutionEngine::JLSR;
				handlers[static_cast<u8>(Opcode::JLE)] = &ExecutionEngine::JLE;
				handlers[static_cast<u8>(Opcode::JLER)] = &ExecutionEngine::JLER;
				handlers[static_cast<u8>(Opcode::JAB)] = &ExecutionEngine::JAB;
				handlers[static_cast<u8>(Opcode::JABR)] = &ExecutionEngine::JABR;
				handlers[static_cast<u8>(Opcode::JAE)] = &ExecutionEngine::JAE;
				handlers[static_cast<u8>(Opcode::JAER)] = &ExecutionEngine::JAER;
				handlers[static_cast<u8>(Opcode::JBL)] = &ExecutionEngine::JBL;
				handlers[static_cast<u8>(Opcode::JBLR)] = &ExecutionEngine::JBLR;
				handlers[static_cast<u8>(Opcode::JBE)] = &ExecutionEngine::JBE;
				handlers[static_cast<u8>(Opcode::JBER)] = &ExecutionEngine::JBER;

				// Stack
				handlers[static_cast<u8>(Opcode::PUSH)] = &ExecutionEngine::PUSH;
				handlers[static_cast<u8>(Opcode::POP)] = &ExecutionEngine::POP;
				handlers[static_cast<u8>(Opcode::PUSHF)] = &ExecutionEngine::PUSHF;
				handlers[static_cast<u8>(Opcode::POPF)] = &ExecutionEngine::POPF;
				handlers[static_cast<u8>(Opcode::LDRX)] = &ExecutionEngine::LDRX;
				handlers[static_cast<u8>(Opcode::LDRBX)] = &ExecutionEngine::LDRBX;
				handlers[static_cast<u8>(Opcode::LDRHX)] = &ExecutionEngine::LDRHX;
				handlers[static_cast<u8>(Opcode::LDRSBX)] = &ExecutionEngine::LDRSBX;
				handlers[static_cast<u8>(Opcode::LDRSHX)] = &ExecutionEngine::LDRSHX;
				handlers[static_cast<u8>(Opcode::FLDRX)] = &ExecutionEngine::FLDRX;
				handlers[static_cast<u8>(Opcode::STRX)] = &ExecutionEngine::STRX;
				handlers[static_cast<u8>(Opcode::STRBX)] = &ExecutionEngine::STRBX;
				handlers[static_cast<u8>(Opcode::STRHX)] = &ExecutionEngine::STRHX;
				handlers[static_cast<u8>(Opcode::FSTRX)] = &ExecutionEngine::FSTRX;
				handlers[static_cast<u8>(Opcode::LDRP)] = &ExecutionEngine::LDRP;
				handlers[static_cast<u8>(Opcode::LDRBP)] = &ExecutionEngine::LDRBP;
				handlers[static_cast<u8>(Opcode::LDRHP)] = &ExecutionEngine::LDRHP;
				handlers[static_cast<u8>(Opcode::LDRSBP)] = &ExecutionEngine::LDRSBP;
				handlers[static_cast<u8>(Opcode::LDRSHP)] = &ExecutionEngine::LDRSHP;
				handlers[static_cast<u8>(Opcode::FLDRP)] = &ExecutionEngine::FLDRP;
				handlers[static_cast<u8>(Opcode::STRP)] = &ExecutionEngine::STRP;
				handlers[static_cast<u8>(Opcode::STRBP)] = &ExecutionEngine::STRBP;
				handlers[static_cast<u8>(Opcode::STRHP)] = &ExecutionEngine::STRHP;
				handlers[static_cast<u8>(Opcode::FSTRP)] = &ExecutionEngine::FSTRP;
				handlers[static_cast<u8>(Opcode::MULH)] = &ExecutionEngine::MULH;
				handlers[static_cast<u8>(Opcode::IMULH)] = &ExecutionEngine::IMULH;
				handlers[static_cast<u8>(Opcode::ABS)] = &ExecutionEngine::ABS;
				handlers[static_cast<u8>(Opcode::MIN)] = &ExecutionEngine::MIN;
				handlers[static_cast<u8>(Opcode::MINI)] = &ExecutionEngine::MINI;
				handlers[static_cast<u8>(Opcode::IMIN)] = &ExecutionEngine::IMIN;
				handlers[static_cast<u8>(Opcode::IMINI)] = &ExecutionEngine::IMINI;
				handlers[static_cast<u8>(Opcode::MAX)] = &ExecutionEngine::MAX;
				handlers[static_cast<u8>(Opcode::MAXI)] = &ExecutionEngine::MAXI;
				handlers[static_cast<u8>(Opcode::IMAX)] = &ExecutionEngine::IMAX;
				handlers[static_cast<u8>(Opcode::IMAXI)] = &ExecutionEngine::IMAXI;
				handlers[static_cast<u8>(Opcode::FMIN)] = &ExecutionEngine::FMIN;
				handlers[static_cast<u8>(Opcode::FMAX)] = &ExecutionEngine::FMAX;
				handlers[static_cast<u8>(Opcode::CLZ)] = &ExecutionEngine::CLZ;
				handlers[static_cast<u8>(Opcode::CTZ)] = &ExecutionEngine::CTZ;
				handlers[static_cast<u8>(Opcode::POPCNT)] = &ExecutionEngine::POPCNT;
				handlers[static_cast<u8>(Opcode::BSWAP)] = &ExecutionEngine::BSWAP;
				handlers[static_cast<u8>(Opcode::ROL)] = &ExecutionEngine::ROL;
				handlers[static_cast<u8>(Opcode::ROLI)] = &ExecutionEngine::ROLI;
				handlers[static_cast<u8>(Opcode::ROR)] = &ExecutionEngine::ROR;
				handlers[static_cast<u8>(Opcode::RORI)] = &ExecutionEngine::RORI;
				handlers[static_cast<u8>(Opcode::SXTB)] = &ExecutionEngine::SXTB;
				handlers[static_cast<u8>(Opcode::SXTH)] = &ExecutionEngine::SXTH;
				handlers[static_cast<u8>(Opcode::FSQRT)] = &ExecutionEngine::FSQRT;
				handlers[static_cast<u8>(Opcode::FABS)] = &ExecutionEngine::FABS;
				handlers[static_cast<u8>(Opcode::FROUND)] = &ExecutionEngine::FROUND;
				handlers[static_cast<u8>(Opcode::FFLOOR)] = &ExecutionEngine::FFLOOR;
				handlers[static_cast<u8>(Opcode::FCEIL)] = &ExecutionEngine::FCEIL;
				handlers[static_cast<u8>(Opcode::FTRUNC)] = &ExecutionEngine::FTRUNC;
				handlers[static_cast<u8>(Opcode::FCOPYSIGN)] = &ExecutionEngine::FCOPYSIGN;
				handlers[static_cast<u8>(Opcode::MCPY)] = &ExecutionEngine::MCPY;
				handlers[static_cast<u8>(Opcode::MSET)] = &ExecutionEngine::MSET;
				handlers[static_cast<u8>(Opcode::MCMP)] = &ExecutionEngine::MCMP;
				handlers[static_cast<u8>(Opcode::MSCAN)] = &ExecutionEngine::MSCAN;
				handlers[static_cast<u8>(Opcode::FMA)] = &ExecutionEngine::FMA;
				handlers[static_cast<u8>(Opcode::FCLASS)] = &ExecutionEngine::FCLASS;
				handlers[static_cast<u8>(Opcode::FRECIPE)] = &ExecutionEngine::FRECIPE;
				handlers[static_cast<u8>(Opcode::FRSQRTE)] = &ExecutionEngine::FRSQRTE;
				handlers[static_cast<u8>(Opcode::ENTER)] = &ExecutionEngine::ENTER;
				handlers[static_cast<u8>(Opcode::LEAVE)] = &ExecutionEngine::LEAVE;
				handlers[static_cast<u8>(Opcode::PUSHM)] = &ExecutionEngine::PUSHM;
				handlers[static_cast<u8>(Opcode::POPM)] = &ExecutionEngine::POPM;
				handlers[static_cast<u8>(Opcode::FPUSH)] = &ExecutionEngine::FPUSH;
				handlers[static_cast<u8>(Opcode::FPOP)] = &ExecutionEngine::FPOP;
				handlers[static_cast<u8>(Opcode::FPUSHM)] = &ExecutionEngine::FPUSHM;
				handlers[static_cast<u8>(Opcode::FPOPM)] = &ExecutionEngine::FPOPM;

				// Conversions
				handlers[static_cast<u8>(Opcode::ITOF)] = &ExecutionEngine::ITOF;
				handlers[static_cast<u8>(Opcode::IITOF)] = &ExecutionEngine::IITOF;
				handlers[static_cast<u8>(Opcode::FTOI)] = &ExecutionEngine::FTOI;
				handlers[static_cast<u8>(Opcode::FTOII)] = &ExecutionEngine::FTOII;
				handlers[static_cast<u8>(Opcode::MTF)] = &ExecutionEngine::MTF;
				handlers[static_cast<u8>(Opcode::MFF)] = &ExecutionEngine::MFF;

				// 64-bit integers
				handlers[static_cast<u8>(Opcode::ADD64)] = &ExecutionEngine::ADD64;
				handlers[static_cast<u8>(Opcode::SUB64)] = &ExecutionEngine::SUB64;
				handlers[static_cast<u8>(Opcode::NEG64)] = &ExecutionEngine::NEG64;
				handlers[static_cast<u8>(Opcode::CMP64)] = &ExecutionEngine::CMP64;
				handlers[static_cast<u8>(Opcode::MULL)] = &ExecutionEngine::MULL;
				handlers[static_cast<u8>(Opcode::IMULL)] = &ExecutionEngine::IMULL;
				handlers[static_cast<u8>(Opcode::MUL64)] = &ExecutionEngine::MUL64;
				handlers[static_cast<u8>(Opcode::DIV64)] = &ExecutionEngine::DIV64;
				handlers[static_cast<u8>(Opcode::IDIV64)] = &ExecutionEngine::IDIV64;
				handlers[static_cast<u8>(Opcode::MOD64)] = &ExecutionEngine::MOD64;
				handlers[static_cast<u8>(Opcode::IMOD64)] = &ExecutionEngine::IMOD64;
				handlers[static_cast<u8>(Opcode::SHL64)] = &ExecutionEngine::SHL64;
				handlers[static_cast<u8>(Opcode::SHR64)] = &ExecutionEngine::SHR64;
				handlers[static_cast<u8>(Opcode::SAR64)] = &ExecutionEngine::SAR64;
				handlers[static_cast<u8>(Opcode::SHI64)] = &ExecutionEngine::SHI64;
				handlers[static_cast<u8>(Opcode::SXT64)] = &ExecutionEngine::SXT64;
				handlers[static_cast<u8>(Opcode::BITS64)] = &ExecutionEngine::BITS64;
				handlers[static_cast<u8>(Opcode::MOV64)] = &ExecutionEngine::MOV64;
				handlers[static_cast<u8>(Opcode::LDRD)] = &ExecutionEngine::LDRD;
				handlers[static_cast<u8>(Opcode::STRD)] = &ExecutionEngine::STRD;
				handlers[static_cast<u8>(Opcode::LDRDX)] = &ExecutionEngine::LDRDX;
				handlers[static_cast<u8>(Opcode::STRDX)] = &ExecutionEngine::STRDX;
				handlers[static_cast<u8>(Opcode::LDRDP)] = &ExecutionEngine::LDRDP;

				// binary64 doubles
				handlers[static_cast<u8>(Opcode::FADDD)] = &ExecutionEngine::FADDD;
				handlers[static_cast<u8>(Opcode::FSUBD)] = &ExecutionEngine::FSUBD;
				handlers[static_cast<u8>(Opcode::FMULD)] = &ExecutionEngine::FMULD;
				handlers[static_cast<u8>(Opcode::FDIVD)] = &ExecutionEngine::FDIVD;
				handlers[static_cast<u8>(Opcode::FMAD)] = &ExecutionEngine::FMAD;
				handlers[static_cast<u8>(Opcode::FSQRTD)] = &ExecutionEngine::FSQRTD;
				handlers[static_cast<u8>(Opcode::FCMPD)] = &ExecutionEngine::FCMPD;
				handlers[static_cast<u8>(Opcode::FMINMAXD)] = &ExecutionEngine::FMINMAXD;
				handlers[static_cast<u8>(Opcode::FMODD)] = &ExecutionEngine::FMODD;
				handlers[static_cast<u8>(Opcode::FUNARYD)] = &ExecutionEngine::FUNARYD;
				handlers[static_cast<u8>(Opcode::FCOPYSIGND)] = &ExecutionEngine::FCOPYSIGND;
				handlers[static_cast<u8>(Opcode::FCLASSD)] = &ExecutionEngine::FCLASSD;
				handlers[static_cast<u8>(Opcode::FCVT)] = &ExecutionEngine::FCVT;
				handlers[static_cast<u8>(Opcode::FMOVD)] = &ExecutionEngine::FMOVD;
				handlers[static_cast<u8>(Opcode::FLDRD)] = &ExecutionEngine::FLDRD;
				handlers[static_cast<u8>(Opcode::FSTRD)] = &ExecutionEngine::FSTRD;
				handlers[static_cast<u8>(Opcode::FLDRDX)] = &ExecutionEngine::FLDRDX;
				handlers[static_cast<u8>(Opcode::FSTRDX)] = &ExecutionEngine::FSTRDX;
				handlers[static_cast<u8>(Opcode::FLDRDP)] = &ExecutionEngine::FLDRDP;
				handlers[static_cast<u8>(Opcode::MTFD)] = &ExecutionEngine::MTFD;
				handlers[static_cast<u8>(Opcode::MFFD)] = &ExecutionEngine::MFFD;

				return handlers;
		}();
	};
}

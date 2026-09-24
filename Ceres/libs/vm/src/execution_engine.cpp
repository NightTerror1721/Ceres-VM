#include <ceres/vm/execution_engine.h>
#include <chrono>

namespace ceres::vm
{
	void ExecutionEngine::reset() noexcept
	{
		_registers = GeneralPurposeRegisterPool();
		_flags = FlagRegister();
		_pc = _memory.readUnchecked<Address::ValueType>(0_addr); // Initialize PC to the value at address 0 (reset vector)
		// Below the system stack, which the top of memory is reserved for.
		_registers.sp() = static_cast<Register::ValueType>(_memory.size() - Memory::SystemStackSize);
		_interruptDepth = 0;
		_savedStackPointer = 0;
		_interruptShadow = false;
		_executedInstructions = 0; // A reset restarts the machine, so its clock restarts with it
		_mmu.reset(); // No program has had the chance to point PTBR at garbage yet; leave none behind either
		_haltCarryNanos = 0;
	}

	namespace
	{
		using HaltClock = std::chrono::steady_clock;

		// The longest one halted step sleeps, so the host loop around step() still gets to see a
		// shutdown, a pause or a window event while a program waits a long time.
		constexpr auto MaxHaltedWait = std::chrono::milliseconds(10);

		HaltClock::duration ticksToDuration(u64 ticks, u64 hz) noexcept
		{
			const u64 seconds = ticks / hz;
			if (seconds > 3600)
				return std::chrono::hours(1);          // far beyond any single wait: MaxHaltedWait caps it anyway
			const u64 nanos = seconds * 1'000'000'000ull + (ticks % hz) * 1'000'000'000ull / hz;
			return std::chrono::duration_cast<HaltClock::duration>(std::chrono::nanoseconds(nanos));
		}

		u64 durationToTicks(HaltClock::duration elapsed, u64 hz) noexcept
		{
			const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
			if (nanos <= 0)
				return 0;
			const u64 ns = static_cast<u64>(nanos);
			return (ns / 1'000'000'000ull) * hz + (ns % 1'000'000'000ull) * hz / 1'000'000'000ull;
		}
	}

	// A halted machine executes nothing, but its clock runs on at `_haltClockHz` ticks per second - an
	// instruction's worth of time per tick, as if the CPU were busy - so a timer armed for N ticks
	// fires N ticks later whether the program waits for it running or halted. The host does not step
	// through those ticks: it sleeps until the next thing a device has scheduled (at most
	// MaxHaltedWait at a time), wakes the moment anything raises an interrupt, and then moves the
	// devices on by the time that actually passed - exactly to the event, when it was reached.
	//
	// With the clock at 0 (a debugger replaying) no real time is involved: a step jumps straight to the
	// next device event, and with none scheduled it waits up to MaxHaltedWait for the host to raise
	// something, then returns so the loop around it can look again.
	void ExecutionEngine::haltedStep(u64 raisesSeen) noexcept
	{
		const u64 toEvent = _mmioBus.ticksUntilNextEvent();
		const HaltClock::time_point start = HaltClock::now();

		if (_haltClockHz == 0)
		{
			if (toEvent != NoDeviceEvent)
				_mmioBus.advance(toEvent);
			else
				_interrupts.waitForRaise(raisesSeen, start + MaxHaltedWait);
			return;
		}

		HaltClock::time_point wakeAt = start + MaxHaltedWait;
		if (toEvent != NoDeviceEvent)
		{
			const HaltClock::duration untilEvent = ticksToDuration(toEvent, _haltClockHz);
			if (untilEvent < MaxHaltedWait)
				wakeAt = start + untilEvent;
		}
		if (wakeAt > start)
			_interrupts.waitForRaise(raisesSeen, wakeAt);

		// The time waited plus what the last steps left over, in whole ticks; the rest carries on.
		const u64 waitedNanos = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(HaltClock::now() - start).count())
			+ _haltCarryNanos;
		u64 ticks = durationToTicks(std::chrono::nanoseconds(waitedNanos), _haltClockHz);
		const u64 usedNanos = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(ticksToDuration(ticks, _haltClockHz)).count());
		_haltCarryNanos = waitedNanos > usedNanos ? waitedNanos - usedNanos : 0;
		if (toEvent != NoDeviceEvent && ticks >= toEvent)
		{
			ticks = toEvent;                      // the event happens on its own tick; the machine then wakes
			_haltCarryNanos = 0;
		}
		_mmioBus.advance(ticks);
	}

	void ExecutionEngine::handleHalt() noexcept
	{
		_flags.set<ExecutionFlag::Halting>(); // Set halting flag to stop execution
		advancePC(); // Advance PC to the next instruction (optional, depending on how you want to handle halting)
	}

	void ExecutionEngine::handleTrap() noexcept
	{
		_flags.set<ExecutionFlag::Trap>(); // Set trap flag to indicate a trap condition
	}

	void ExecutionEngine::triggerInterrupt(InterruptNumber interruptNumber) noexcept
	{
		// Captured before anything can redirect it: for a fault this is the instruction that
		// caused it, and it is the one thing a debugger cannot recover afterwards.
		const Address faultingPC = _pc;
		const auto notify = [&](bool entered) noexcept
		{
			if (_interruptObserver)
				_interruptObserver(interruptNumber, faultingPC, entered);
		};

		if (!_flags.get<ExecutionFlag::Interrupt>() && static_cast<u8>(interruptNumber) >= ReservedInterruptCount)
		{
			notify(false);
			return; // Ignore interrupts if interrupt flag is not set or if the interrupt number is reserved
		}

		const Address interruptVectorAddress = Address(static_cast<Address::ValueType>(interruptNumber) * Address::Size);
		const u32 handlerAddress = _memory.readUnchecked<u32>(interruptVectorAddress);

		if (handlerAddress == 0)
		{
			notify(false);
			return; // Ignore if no handler is defined
		}

		// The handler runs on the system stack, not on whatever the program had left. Only the
		// outermost interrupt switches: a nested one is already there. The program's own stack
		// pointer is put back by the matching IRET, which also means a handler that leaves the
		// stack unbalanced cannot corrupt the program it interrupted.
		if (_interruptDepth == 0)
		{
			_savedStackPointer = sp();
			sp(static_cast<u32>(_memory.size()));
		}
		++_interruptDepth;

		// Saving state needs two words. If they do not fit, this dispatch would push, overflow, and
		// re-enter here forever. Stop the machine instead: there is nowhere left to record what
		// happened, so continuing can only make it worse.
		if (!hasStackRoom(2 * sizeof(u32)))
		{
			_flags.set<ExecutionFlag::Trap>();
			_flags.set<ExecutionFlag::Halting>();
			leaveInterrupt();
			notify(false);
			return;
		}

		// The halting flag is deliberately left out of the saved state. HALT means "wait for an
		// interrupt", so once one has been serviced the wait is over: restoring the bit on IRET
		// would put the machine straight back to sleep and no device could ever wake it.
		const FlagRegister savedFlags{ _flags.value() & ~static_cast<FlagRegister::ValueType>(ExecutionFlag::Halting) };

		push<u32>(savedFlags.value());
		push<u32>(_pc.value());

		_flags.clear<ExecutionFlag::Interrupt>(); // Clear interrupt flag before handling the interrupt
		_flags.clear<ExecutionFlag::Halting>(); // Clear halting flag to allow execution to continue after handling the interrupt

		_pc = Address(handlerAddress);
		// The redirect just above is the one PC write a still-running handler must not undo: this is
		// what advancePC() (and push<T>/pop<T>, for the handlers that read a value back through them)
		// check to stay out of its way for the rest of the instruction that triggered it.
		_faulted = true;
		notify(true);
	}

	void ExecutionEngine::step() noexcept
	{
		// Pending requests are delivered before anything else, and this is what lets a device
		// wake a halted machine: triggerInterrupt clears the halting flag.
		// The instruction after an STI runs before user interrupts can be delivered (see
		// _interruptShadow). The reserved ones are faults and traps, which no mask ever held back.
		const bool shadowed = _interruptShadow;
		_interruptShadow = false;

		// Taken before the pending request is looked at: a halted step that sleeps below must be woken by
		// anything raised from here on, including a raise between this look and the sleep.
		const u64 raisesSeen = _interrupts.raiseCount();

		if (const auto pending = _interrupts.peek(); pending.has_value())
		{
			// A masked interrupt stays queued rather than being thrown away.
			const bool deliverable = (_flags.get<ExecutionFlag::Interrupt>() && !shadowed) ||
				static_cast<u8>(pending.value()) < ReservedInterruptCount;

			if (deliverable)
			{
				_interrupts.clear(pending.value());
				triggerInterrupt(pending.value());
			}
		}

		if (_flags.get<ExecutionFlag::Halting>())
		{
			haltedStep(raisesSeen);
			return;
		}

		// Counted before the instruction runs, because running it is what moves the PC. Profiling
		// is off unless enableProfiling() was called, so this is cold on every step() by default -
		// same rationale as translate()'s [[likely]] on the paging-off path.
		if (_executionCountsData && _pc.value() >= _textStart && _pc.value() < _textEnd) [[unlikely]]
			++_executionCountsData[(_pc.value() - _textStart) / Instruction::Size];

		_faulted = false;
		const Instruction instruction = fetch();
		// A fetch that page-faulted already redirected the PC to the handler; the word it "fetched"
		// is a dummy that must never run, or the machine would execute whatever raw bits happened to
		// sit at that virtual address's physical counterpart instead of the fault handler.
		if (!_faulted) [[likely]]
			execute(instruction);
		++_executedInstructions;
		_mmioBus.tick();
	}
}

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
		_raisesConsumed = _interrupts.raiseCount(); // what the last program raised does not wake this one
		_stoppedForGood = false;
		_faultAddress = 0;
		_faultAccess = 0;
		_cycles = 0;
		_faultReason = FaultReason::None;
		_stackLimit = _stackFloor;                  // the heap the limit guarded starts again from nothing
		_executedInstructions = 0; // A reset restarts the machine, so its clock restarts with it
		_mmu.reset(); // No program has had the chance to point PTBR at garbage yet; leave none behind either
	}

	namespace
	{
		// The longest one halted step waits for the host when no device has anything scheduled, so the loop
		// around step() still gets to see a shutdown, a pause or a window event.
		constexpr auto MaxHaltedWait = std::chrono::milliseconds(10);
	}

	// A halted machine executes nothing, and its clock jumps straight to the next device event (plan/v2 SPEC
	// 3.2): time is the machine's own, and how it keeps pace with the host is the runner's business. With
	// nothing scheduled only the host can wake it - a key, a raise from a device's thread - so the step waits
	// for one, a little at a time, and returns so the loop around it can look again.
	void ExecutionEngine::haltedStep(u64 raisesSeen) noexcept
	{
		Scheduler& scheduler = _mmioBus.scheduler();
		const u64 next = scheduler.nextCycle();
		if (next == NoScheduledEvent)
		{
			_interrupts.waitForRaise(raisesSeen, std::chrono::steady_clock::now() + MaxHaltedWait);
			return;
		}
		if (next > _cycles)
			_cycles = next;
		scheduler.service(_cycles);
	}

	void ExecutionEngine::handleHalt() noexcept
	{
		// A request raised since the last wake is the event this HALT waits for, already here: it is
		// spent and the machine goes on (see _raisesConsumed). Otherwise it sleeps until the next one.
		const u64 raises = _interrupts.raiseCount();
		if (raises != _raisesConsumed)
			_raisesConsumed = raises;
		else
			_flags.set<ExecutionFlag::Halting>();
		advancePC();
	}

	void ExecutionEngine::handleTrap() noexcept
	{
		_flags.set<ExecutionFlag::Trap>(); // Set trap flag to indicate a trap condition
	}

	bool ExecutionEngine::triggerInterrupt(InterruptNumber interruptNumber) noexcept
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
			return false; // Ignore interrupts if interrupt flag is not set or if the interrupt number is reserved
		}

		const Address interruptVectorAddress = Address(static_cast<Address::ValueType>(interruptNumber) * Address::Size);
		const u32 handlerAddress = _memory.readUnchecked<u32>(interruptVectorAddress);

		if (handlerAddress == 0)
		{
			notify(false);
			return false; // Ignore if no handler is defined
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
			_stoppedForGood = true;
			leaveInterrupt();
			notify(false);
			return false;
		}

		// The halting flag is deliberately left out of the saved state. HALT means "wait for an
		// interrupt", so once one has been serviced the wait is over: restoring the bit on IRET
		// would put the machine straight back to sleep and no device could ever wake it.
		const FlagRegister savedFlags{ _flags.value() & ~static_cast<FlagRegister::ValueType>(ExecutionFlag::Halting) };

		const u64 entryStart = _cycles;
		push<u32>(savedFlags.value());
		push<u32>(_pc.value());
		_cycles = entryStart + isa::cycles::InterruptEntry;  // the two pushes are part of it

		_flags.clear<ExecutionFlag::Interrupt>(); // Clear interrupt flag before handling the interrupt
		_flags.clear<ExecutionFlag::Halting>(); // Clear halting flag to allow execution to continue after handling the interrupt

		_pc = Address(handlerAddress);
		// The redirect just above is the one PC write a still-running handler must not undo: this is
		// what advancePC() (and push<T>/pop<T>, for the handlers that read a value back through them)
		// check to stay out of its way for the rest of the instruction that triggered it.
		_faulted = true;
		notify(true);
		return true;
	}

	void ExecutionEngine::step() noexcept
	{
		// Pending requests are delivered before anything else, and this is what lets a device
		// wake a halted machine: triggerInterrupt clears the halting flag.
		// The instruction after an STI runs before user interrupts can be delivered (see
		// _interruptShadow). The reserved ones are faults and traps, which no mask ever held back.
		const bool shadowed = _interruptShadow;
		_interruptShadow = false;

		// A device event that came due during the last instruction runs now, before the pending requests are
		// looked at: whatever it raises is delivered on this step, as if it had run right after that instruction.
		if (_cycles >= _nextEvent) [[unlikely]]
			_mmioBus.scheduler().service(_cycles);

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
				// Taking an interrupt is the wake-up: the HALT after its IRET waits for the next one. One
				// dropped for want of a handler is not taken, and still wakes a halt below.
				if (triggerInterrupt(pending.value()))
					_raisesConsumed = raisesSeen;
			}
		}

		if (_flags.get<ExecutionFlag::Halting>())
		{
			// A request raised since the machine went to sleep ends the halt even when it was not taken
			// - masked, or with no handler to take it (vector 0). A machine stopped by a fault it had no
			// stack left to report stays stopped.
			if (raisesSeen != _raisesConsumed && !_stoppedForGood)
			{
				_raisesConsumed = raisesSeen;
				_flags.clear<ExecutionFlag::Halting>();
			}
			else
			{
				haltedStep(raisesSeen);
				return;
			}
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
		{
			_cycles += isa::cycles::Base[static_cast<u8>(instruction.opcode())];
			execute(instruction);
		}
		++_executedInstructions;

	}
}

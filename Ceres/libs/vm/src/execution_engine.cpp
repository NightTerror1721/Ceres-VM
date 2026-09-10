#include <ceres/vm/execution_engine.h>
#include <thread>

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
		_executedInstructions = 0; // A reset restarts the machine, so its clock restarts with it
		_mmu.reset(); // No program has had the chance to point PTBR at garbage yet; leave none behind either
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
		if (const auto pending = _interrupts.peek(); pending.has_value())
		{
			// A masked interrupt stays queued rather than being thrown away.
			const bool deliverable = _flags.get<ExecutionFlag::Interrupt>() ||
				static_cast<u8>(pending.value()) < ReservedInterruptCount;

			if (deliverable)
			{
				_interrupts.clear(pending.value());
				triggerInterrupt(pending.value());
			}
		}

		if (_flags.get<ExecutionFlag::Halting>())
		{
			// Nothing to run, but devices still keep time: this is how a timer eventually fires.
			_mmioBus.tick();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return;
		}

		// Counted before the instruction runs, because running it is what moves the PC.
		if (_executionCountsData && _pc.value() >= _textStart && _pc.value() < _textEnd)
			++_executionCountsData[(_pc.value() - _textStart) / Instruction::Size];

		_faulted = false;
		const Instruction instruction = fetch();
		// A fetch that page-faulted already redirected the PC to the handler; the word it "fetched"
		// is a dummy that must never run, or the machine would execute whatever raw bits happened to
		// sit at that virtual address's physical counterpart instead of the fault handler.
		if (!_faulted)
			execute(instruction);
		++_executedInstructions;
		_mmioBus.tick();
	}
}

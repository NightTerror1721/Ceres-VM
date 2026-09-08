#include "execution_engine.h"
#include "interrupts.h"
#include <thread>

namespace ceres::vm
{
	void ExecutionEngine::reset() noexcept
	{
		_registers = GeneralPurposeRegisterPool();
		_flags = FlagRegister();
		_pc = _memory.readUnchecked<Address::ValueType>(0_addr); // Initialize PC to the value at address 0 (reset vector)
		_registers.sp() = static_cast<Register::ValueType>(_memory.size()); // Initialize stack pointer to the end of memory
		_executedInstructions = 0; // A reset restarts the machine, so its clock restarts with it
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

		// Saving state needs two words. If they do not fit, this dispatch would push, overflow, and
		// re-enter here forever. Stop the machine instead: there is nowhere left to record what
		// happened, so continuing can only make it worse.
		if (!hasStackRoom(2 * sizeof(u32)))
		{
			_flags.set<ExecutionFlag::Trap>();
			_flags.set<ExecutionFlag::Halting>();
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
			_ioPorts.tick();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return;
		}

		// Counted before the instruction runs, because running it is what moves the PC.
		if (!_executionCounts.empty() && _pc.value() >= _textStart && _pc.value() < _textEnd)
			++_executionCounts[(_pc.value() - _textStart) / Instruction::Size];

		const Instruction instruction = fetch();
		execute(instruction);
		++_executedInstructions;
		_ioPorts.tick();
	}
}
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
		if (!_flags.get<ExecutionFlag::Interrupt>() && static_cast<u8>(interruptNumber) >= ReservedInterruptCount)
			return; // Ignore interrupts if interrupt flag is not set or if the interrupt number is reserved

		const Address interruptVectorAddress = Address(static_cast<Address::ValueType>(interruptNumber) * Address::Size);
		const u32 handlerAddress = _memory.readUnchecked<u32>(interruptVectorAddress);

		if (handlerAddress == 0)
			return; // Ignore if no handler is defined

		push<u32>(_flags.value());
		push<u32>(_pc.value());

		_flags.clear<ExecutionFlag::Interrupt>(); // Clear interrupt flag before handling the interrupt
		_flags.clear<ExecutionFlag::Halting>(); // Clear halting flag to allow execution to continue after handling the interrupt
		
		_pc = Address(handlerAddress);
	}

	void ExecutionEngine::step() noexcept
	{
		if (_flags.get<ExecutionFlag::Halting>())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Sleep briefly to prevent busy-waiting while halted
			return; // Do not execute if halting or trap flag is set
		}

		const Instruction instruction = fetch();
		execute(instruction);
	}
}
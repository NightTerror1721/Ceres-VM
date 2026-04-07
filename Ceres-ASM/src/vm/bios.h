#pragma once

#include "address.h"
#include "instructions.h"
#include "memory.h"
#include "interrupts.h"

namespace ceres::vm
{
	class BIOS
	{
	public:
		static inline constexpr Address ResetVectorAddress = 0_addr; // Address where the reset vector is located (initial PC value on reset)

	public:
		BIOS() = default;
		BIOS(const BIOS&) = default;
		BIOS(BIOS&&) = default;
		~BIOS() = default;

		BIOS& operator=(const BIOS&) = default;
		BIOS& operator=(BIOS&&) = default;

	public:
		void initializeMemory(Memory& memory) const noexcept
		{
			for (InterruptNumber in = InterruptNumber::Trap; in <= InterruptNumber::UserInterrupt0; in = static_cast<InterruptNumber>(static_cast<u8>(in) + 1))
				initVector(memory, in, 0);

			write(memory, 0, Instruction::MOVI(0, 'E'));
			write(memory, 1, Instruction::OUT(0, 0x1));
			write(memory, 2, Instruction::HALT());
		}

	private:
		static inline void write(Memory& memory, usize offset, Instruction instruction) noexcept
		{
			const Address address = Memory::BiosSegmentStart + Address(static_cast<Address::ValueType>(offset * Instruction::Size));
			memory.writeUnchecked<u32>(address, instruction.raw());
		}

		static inline void initVector(Memory& memory, InterruptNumber interruptNumber, usize biosMemoryOffset) noexcept
		{
			const Address vectorAddress = Memory::NullPageSegmentStart + Address(static_cast<Address::ValueType>(static_cast<u8>(interruptNumber) * Address::Size));
			const Address handlerAddress = Memory::BiosSegmentStart + Address(static_cast<Address::ValueType>(biosMemoryOffset * Instruction::Size));
			memory.writeUnchecked<u32>(vectorAddress, handlerAddress.value());
		}
	};
}

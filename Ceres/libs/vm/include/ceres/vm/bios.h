#pragma once

#include <ceres/core/isa/address.h>
#include <ceres/core/isa/instructions.h>
#include <ceres/core/isa/interrupts.h>
#include "memory.h"
#include "mmio_bus.h"

namespace ceres::vm
{
	using namespace isa;

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

			// Ports are gone, so reaching the terminal takes building its MMIO address in a
			// register first: r1 = default_mmio::Terminal + TerminalDevice::OutputRegister
			// (0xFF000004). devices.h cannot be included from here - devices depends on vm, not
			// the other way round - so the +0x04 is spelled out rather than named.
			write(memory, 0, Instruction::LI(0, 'E'));
			write(memory, 1, Instruction::LUI(1, static_cast<u16>(default_mmio::Terminal.value() >> 16)));
			write(memory, 2, Instruction::ORI(1, 1, 0x0004));
			write(memory, 3, Instruction::STRB(1, 0, 0));
			write(memory, 4, Instruction::HALT());
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

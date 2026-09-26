#pragma once

#include <ceres/core/isa/address.h>
#include <ceres/core/isa/instructions.h>
#include <ceres/core/isa/interrupts.h>
#include "memory.h"
#include "mmio_bus.h"

#include <optional>

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
		// The vectors the BIOS gives a default handler: an exception (or the timer's interrupt) that the program
		// did not bind ends the run, with the status a C program reports on a fault.
		static inline constexpr InterruptNumber FirstDefaultVector = InterruptNumber::Trap;
		static inline constexpr InterruptNumber LastDefaultVector = InterruptNumber::UserInterrupt0;
		static inline constexpr u8 UnhandledExitStatus = 1;
		// Each vector has an entry of its own, so where the machine stopped says which exception nobody handled.
		static inline constexpr usize EntryInstructions = 6;
		static_assert(Memory::BiosSegmentStart.value() + (static_cast<usize>(LastDefaultVector) - static_cast<usize>(FirstDefaultVector) + 1) * EntryInstructions * Instruction::Size <= Memory::UnrestrictedSegmentStart.value(),
			"the default handlers must fit in the BIOS segment");

		void initializeMemory(Memory& memory) const noexcept
		{
			// Each entry shuts the machine down through the system control device's Command register, with the
			// exit status in the byte above the command (r1 = default_mmio::SystemControl + 0x00, r0 = 0x0101).
			// devices.h cannot be included from here - devices depends on vm, not the other way round - so the
			// register and the command are spelled out rather than named. Should nothing be attached there to
			// shut it down, the entry halts for good: a wake-up goes back to the HALT, not into the next entry.
			constexpr u32 control = default_mmio::SystemControl.value();
			constexpr u16 shutdown = static_cast<u16>(0x01 | (UnhandledExitStatus << 8));
			for (u8 in = static_cast<u8>(FirstDefaultVector); in <= static_cast<u8>(LastDefaultVector); ++in)
			{
				const usize entry = (in - static_cast<u8>(FirstDefaultVector)) * EntryInstructions;
				initVector(memory, static_cast<InterruptNumber>(in), entry);
				write(memory, entry + 0, Instruction::LUI(1, static_cast<u16>(control >> 16)));
				write(memory, entry + 1, Instruction::ORI(1, 1, static_cast<u16>(control & 0xFFFF)));
				write(memory, entry + 2, Instruction::LI(0, shutdown));
				write(memory, entry + 3, Instruction::STR(1, 0, 0));
				write(memory, entry + 4, Instruction::HALT());
				write(memory, entry + 5, Instruction::JP(i24(-static_cast<i32>(Instruction::Size))));
			}
		}

		// The vector whose default handler holds `pc`, or nothing when `pc` is outside them: what a host asks once
		// the machine has stopped, to tell an exception nobody handled from a program that shut down on its own.
		static constexpr std::optional<InterruptNumber> defaultHandlerVector(Address pc) noexcept
		{
			constexpr u32 entrySize = static_cast<u32>(EntryInstructions * Instruction::Size);
			constexpr u32 count = static_cast<u32>(LastDefaultVector) - static_cast<u32>(FirstDefaultVector) + 1;
			const u32 start = Memory::BiosSegmentStart.value();
			if (pc.value() < start || pc.value() >= start + count * entrySize)
				return std::nullopt;
			return static_cast<InterruptNumber>(static_cast<u32>(FirstDefaultVector) + (pc.value() - start) / entrySize);
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

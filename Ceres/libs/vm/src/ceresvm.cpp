#include <ceres/vm/ceresvm.h>
#include <array>

namespace ceres::vm
{
	std::expected<void, std::string> CeresVM::loadProgram(const Program& program) noexcept
	{
		if (isPoweredOn())
			return std::unexpected("Cannot load a program while the VM is powered on. Please reset the VM before loading a new program.");

		const ProgramHeader& header = program.header();

		const usize requiredMemory = Memory::UnrestrictedSegmentStartValue +
			header.textSize +
			header.rodataSize +
			header.dataSize +
			header.bssSize +
			header.minimumStack;

		if (requiredMemory > _memory.size())
		{
			return std::unexpected("Program requires at least " + std::to_string(requiredMemory) +
				" bytes of memory, but only " + std::to_string(_memory.size()) + " bytes are available.");
		}

		// Validate metadata before changing VM memory, so a rejected program cannot leave a
		// partially replaced image behind.
		std::array<bool, isa::InterruptNumberCount> patchedVectors{};
		for (const auto& patch : program.interruptVectors())
		{
			if (patch.interruptNumber == 0 || patch.interruptNumber >= isa::InterruptNumberCount)
				return std::unexpected("Invalid .cres: interrupt vector patch targets out-of-range interrupt " + std::to_string(patch.interruptNumber) + ".");
			if (patchedVectors[patch.interruptNumber])
				return std::unexpected("Invalid .cres: interrupt vector " + std::to_string(patch.interruptNumber) + " is patched more than once.");
			patchedVectors[patch.interruptNumber] = true;
		}

		Address offset = Memory::UnrestrictedSegmentStart;

		// Where .text lands, so the engine can refuse to let the program write over it.
		const Address textStart = offset;

		if (header.textSize > 0)
		{
			_memory.writeBytesUnchecked(offset, program.text());
			offset += header.textSize;
		}
		_engine.setTextRange(textStart.value(), textStart.value() + header.textSize);

		if (header.rodataSize > 0)
		{
			_memory.writeBytesUnchecked(offset, program.rodata());
			offset += header.rodataSize;
		}

		if (header.dataSize > 0)
		{
			_memory.writeBytesUnchecked(offset, program.data());
			offset += header.dataSize;
		}

		// Memory is only zero-initialized at VM construction. Reloading a program must not expose
		// the previous image's bytes through its BSS.
		_memory.setBytesUnchecked(offset, 0, header.bssSize);
		offset += header.bssSize;

		// Everything from here up is free ground: heap first, then the stack coming down from the
		// top of memory. The machine could only ever guard the vector table and the BIOS before,
		// because nothing told it where the image ended - and this loop has known all along.
		_engine.setStackLimit(offset.value());

		_bios.initializeMemory(_memory);
		_memory.writeUnchecked<u32>(0_addr, header.entryPoint); // Write the entry point to the null page so that the execution engine can read it on reset.

		// Applied after the BIOS default table, so a vector nothing bound still falls through to the
		// shared stub exactly as it always has - only the bound ones get overwritten. `writeUnchecked`
		// is what a running program can never do for itself: the checked accessors every instruction
		// goes through refuse this whole region, precisely so a stray pointer can't reach it.
		for (const auto& patch : program.interruptVectors())
		{
			const Address vectorAddress = Memory::NullPageSegmentStart + Address(static_cast<Address::ValueType>(patch.interruptNumber) * Address::Size);
			_memory.writeUnchecked<u32>(vectorAddress, patch.handlerAddress);
		}

		_engine.reset(); // Reset the execution engine to set the PC to the entry point and initialize registers/flags.

		return {};
	}

	std::expected<void, std::string> CeresVM::powerOn() noexcept
	{
		if (isPoweredOn())
			return std::unexpected("VM is already powered on. Please reset the VM before running a new program.");

		_isPoweredOn.store(true, std::memory_order_release);
		return {};
	}

	std::expected<void, std::string> CeresVM::run() noexcept
	{
		if (auto powered = powerOn(); !powered)
			return powered;

		while (isPoweredOn())
			_engine.step();

		return {};
	}
}

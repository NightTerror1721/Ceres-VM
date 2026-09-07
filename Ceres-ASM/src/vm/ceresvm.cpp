#include "ceresvm.h"

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

		Address offset = Memory::UnrestrictedSegmentStart;

		if (header.textSize > 0)
		{
			_memory.writeBytesUnchecked(offset, program.text());
			offset += header.textSize;
		}

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

		offset += header.bssSize; // BSS is zero-initialized, so we just need to reserve the space.

		_bios.initializeMemory(_memory);
		_memory.writeUnchecked<u32>(0_addr, header.entryPoint); // Write the entry point to the null page so that the execution engine can read it on reset.

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
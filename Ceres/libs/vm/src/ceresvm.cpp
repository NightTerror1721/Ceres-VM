#include <ceres/vm/ceresvm.h>
#include <array>

namespace ceres::vm
{
	usize CeresVM::argumentBlockSize() const noexcept
	{
		usize strings = 0;
		for (const std::string& text : _arguments.arguments)
			strings += text.size() + 1;
		for (const std::string& text : _arguments.environment)
			strings += text.size() + 1;
		const usize pointers = (_arguments.arguments.size() + 1 + _arguments.environment.size() + 1) * sizeof(u32);
		return ((strings + 3) & ~usize{ 3 }) + pointers + 8;   // the strings word-aligned, and sp 8-aligned below
	}

	void CeresVM::placeArguments() noexcept
	{
		// From the top of the program's stack down: the strings, then argv[] and envp[] below them, and sp
		// under those - as a hosted C implementation starts main.
		const u32 top = static_cast<u32>(_memory.size() - Memory::SystemStackSize);
		usize strings = 0;
		for (const std::string& text : _arguments.arguments)
			strings += text.size() + 1;
		for (const std::string& text : _arguments.environment)
			strings += text.size() + 1;
		const u32 stringsAt = static_cast<u32>((top - strings) & ~usize{ 3 });
		const u32 argc = static_cast<u32>(_arguments.arguments.size());
		const u32 envc = static_cast<u32>(_arguments.environment.size());
		const u32 vectorAt = (stringsAt - (argc + 1 + envc + 1) * 4u) & ~7u;
		const u32 environmentAt = vectorAt + (argc + 1) * 4u;

		u32 cursor = stringsAt;
		auto put = [&](const std::string& text, u32 slot)
		{
			_memory.writeBytesUnchecked(Address(cursor), std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
			_memory.writeUnchecked<u8>(Address(cursor + static_cast<u32>(text.size())), 0);
			_memory.writeUnchecked<u32>(Address(slot), cursor);
			cursor += static_cast<u32>(text.size()) + 1;
		};
		for (u32 i = 0; i < argc; ++i)
			put(_arguments.arguments[i], vectorAt + i * 4u);
		_memory.writeUnchecked<u32>(Address(vectorAt + argc * 4u), 0);
		for (u32 i = 0; i < envc; ++i)
			put(_arguments.environment[i], environmentAt + i * 4u);
		_memory.writeUnchecked<u32>(Address(environmentAt + envc * 4u), 0);

		_argumentBlock = ArgumentBlock{ argc, vectorAt, environmentAt };
		_engine.setRegister(0, argc);
		_engine.setRegister(1, vectorAt);
		_engine.setRegister(2, environmentAt);
		_engine.setRegister(15, vectorAt);            // sp: the arrays and strings stay above the stack
	}

	std::expected<void, std::string> CeresVM::loadProgram(const Program& program) noexcept
	{
		if (isPoweredOn())
			return std::unexpected("Cannot load a program while the VM is powered on. Please reset the VM before loading a new program.");

		const ProgramHeader& header = program.header();

		// The image, the program's own stack, and the system stack at the top that interrupt handlers run
		// on: the image must end below systemStackFloor() with minimumStack to spare.
		const usize requiredMemory = Memory::UnrestrictedSegmentStartValue +
			header.textSize +
			header.rodataSize +
			header.dataSize +
			header.bssSize +
			header.minimumStack +
			Memory::SystemStackSize +
			argumentBlockSize();

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
		placeArguments();

		// Kept for a reset. Skipped when a reset is reloading this very program.
		if (!_program.has_value() || &*_program != &program)
			_program = program;

		return {};
	}

	bool CeresVM::restartIfRequested() noexcept
	{
		// requestReset() may run on another thread: this pairs with its release store of the power-off
		// the step loop saw, so the request stored before it is seen here too.
		std::atomic_thread_fence(std::memory_order_acquire);
		if (!_resetRequested.exchange(false, std::memory_order_acq_rel) || !_program.has_value())
			return false;

		// Powered off by requestReset(), so loadProgram() accepts it. It already validated this image once.
		if (!loadProgram(*_program))
			return false;
		_interrupts.clearAll();       // requests raised for the program that just ended
		_mmioBus.resetDevices();
		_isPoweredOn.store(true, std::memory_order_release);
		return true;
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
		// A reset asked for before run() is carried out first; otherwise the machine starts as it is.
		if (!restartIfRequested())
		{
			if (auto powered = powerOn(); !powered)
				return powered;
		}

		// A reset stops the inner loop like a shutdown does; it starts again only if one was asked for.
		do
		{
			while (isPoweredOn())
				_engine.step();
		} while (restartIfRequested());

		return {};
	}
}

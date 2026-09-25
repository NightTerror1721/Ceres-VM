#include <ceres/devices/terminal/terminal.h>
#include <print>

namespace ceres::devices
{
	namespace
	{
		// Every register of the device (plan/v2 SPEC 5.3), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x00, "Status",         RegisterAccess::Read,      0x2, false, "Bit 0 input available, bit 1 ready for output, bit 2 end of input." },
			{ 0x04, "Output",         RegisterAccess::Write,     0x0, false, "The low byte goes to the output stream." },
			{ 0x08, "Input",          RegisterAccess::Read,      0x0, true,  "The next input byte, or 0 when there is none." },
			{ 0x0C, "BytesAvailable", RegisterAccess::Read,      0x0, false, "Bytes currently sitting unread in the input ring." },
			{ 0x10, "BlockReadCount", RegisterAccess::Read,      0x0, false, "Bytes the most recent block-read actually moved into RAM." },
			{ 0x14, "DroppedInput",   RegisterAccess::Read,      0x0, false, "Input bytes discarded by a full ring (truncated to 32 bits)." },
			{ 0x18, "Mode",           RegisterAccess::ReadWrite, 0x0, false, "Write ModeRaw to ask the host for keys as they are pressed (no line editing, no echo); read what the host granted." },
			{ 0x1C, "ErrorOutput",    RegisterAccess::Write,     0x0, false, "The low byte goes to the error stream." },
			{ 0xF0, "BlockAddress",   RegisterAccess::Write,     0x0, false, "RAM address of a block transfer." },
			{ 0xF4, "BlockLength",    RegisterAccess::Write,     0x0, false, "Bytes in a block transfer." },
			{ 0xF8, "BlockCommand",   RegisterAccess::Write,     0x0, false, "1 reads input into RAM, 2 writes RAM to the output, 3 to the error stream." },
		};
	}

	void TerminalDevice::pushInput(std::span<const u8> input)
	{
		const std::lock_guard lock{_inputMutex};
		bool wroteAnyByte = false;
		for (u8 byte : input)
		{
			usize nextTail = (_tail.load(std::memory_order_relaxed) + 1) % InputBufferCapacity;
			if (nextTail == _head.load(std::memory_order_acquire))
			{
				_droppedInputBytes.fetch_add(1, std::memory_order_relaxed);
				continue;
			}

			_buffer[_tail.load(std::memory_order_relaxed)] = byte;
			_tail.store(nextTail, std::memory_order_release);
			wroteAnyByte = true;
		}

		// Idempotent if the machine is already awake or a request is already pending - raise()
		// only sets a bit, and triggerInterrupt() clears it once delivered - so calling this once
		// per pushInput() rather than once per byte costs nothing and wakes a halted CPU exactly
		// as reliably.
		if (wroteAnyByte)
			raiseInterrupt(Interrupt);
	}

	void TerminalDevice::closeInput()
	{
		_inputClosed.store(true, std::memory_order_release);
		raiseInterrupt(Interrupt);
	}

	usize TerminalDevice::availableBytes() const noexcept
	{
		const usize head = _head.load(std::memory_order_acquire);
		const usize tail = _tail.load(std::memory_order_acquire);
		return (tail - head + InputBufferCapacity) % InputBufferCapacity;
	}

	TerminalDevice::State TerminalDevice::captureState() const noexcept
	{
		const std::lock_guard lock{_inputMutex};
		State state;
		state.buffer = _buffer;
		state.head = _head.load(std::memory_order_acquire);
		state.tail = _tail.load(std::memory_order_acquire);
		state.closed = _inputClosed.load(std::memory_order_acquire);
		return state;
	}

	void TerminalDevice::restoreState(const State& state) noexcept
	{
		const std::lock_guard lock{_inputMutex};
		_buffer = state.buffer;
		_head.store(state.head, std::memory_order_release);
		_tail.store(state.tail, std::memory_order_release);
		_inputClosed.store(state.closed, std::memory_order_release);
	}

	void TerminalDevice::emitByte(u8 value)
	{
		if (_outputSink)
		{
			_outputSink(value);
			return;
		}

		// Cast to char (not just u8) so std::format picks the character formatter: the
		// integer formatter's 'c' presentation additionally demands the value fit in a
		// *signed* char, which throws format_error and aborts the process for any byte
		// >= 0x80 — i.e. any accented letter or multi-byte UTF-8 sequence.
		std::print("{:c}", static_cast<char>(value));
	}

	void TerminalDevice::emitErrorByte(u8 value)
	{
		if (_errorSink)
		{
			_errorSink(value);
			return;
		}
		std::fputc(value, stderr);
	}

	void TerminalDevice::blockRead(Address ramAddress, u32 size)
	{
		if (size == 0)
		{
			_blockReadCount = 0;
			return;
		}

		const std::lock_guard lock{_inputMutex};
		// Clamp to RAM the store is allowed to touch: a BLOCK_ADDR/BLOCK_LEN past the end of
		// memory (or into a protected segment) moves fewer bytes rather than throwing.
		const u32 clampSize = memory().clampBlockSize(ramAddress, size);
		if (clampSize == 0)
		{
			_blockReadCount = 0;
			return;
		}

		auto buffer = memory().peekMutBytes(ramAddress, clampSize);
		usize bytesRead = 0;
		while (bytesRead < buffer.size())
		{
			usize currentHead = _head.load(std::memory_order_relaxed);
			if (currentHead == _tail.load(std::memory_order_acquire))
				break; // No more input available.

			buffer[bytesRead] = _buffer[currentHead];
			_head.store((currentHead + 1) % InputBufferCapacity, std::memory_order_release);
			++bytesRead;
		}

		// A short read - fewer bytes buffered than asked for - is now observable: the program
		// reads this back afterwards and knows exactly where its input ended.
		_blockReadCount = static_cast<u32>(bytesRead);
	}

	void TerminalDevice::blockWrite(Address ramAddress, u32 size, bool error)
	{
		if (size == 0)
			return;

		const u32 clampSize = memory().clampBlockSize(ramAddress, size);
		if (clampSize == 0)
			return;

		const auto buffer = memory().peekBytes(ramAddress, clampSize);
		for (u32 i = 0; i < buffer.size(); ++i)
		{
			if (error)
				emitErrorByte(buffer[i]);
			else
				emitByte(buffer[i]);
		}
	}

	u32 TerminalDevice::readStatusOrInput(Address offset)
	{
		if (offset == StatusRegister)
		{
			u32 status = 0;
			if (_head.load(std::memory_order_acquire) != _tail.load(std::memory_order_acquire))
				status |= RxReadyMask; // Set RxReady if input is available.
			else if (_inputClosed.load(std::memory_order_acquire))
				status |= EofMask; // Closed and drained: nothing more will ever arrive.
			status |= TxReadyMask; // Terminal is always ready to accept output.
			return status;
		}

		if (offset == InputRegister)
		{
			const std::lock_guard lock{_inputMutex};
			usize currentHead = _head.load(std::memory_order_relaxed);
			if (currentHead == _tail.load(std::memory_order_acquire))
				return 0; // No input available, return 0.

			const u32 value = _buffer[currentHead];
			_head.store((currentHead + 1) % InputBufferCapacity, std::memory_order_release);
			return value;
		}

		return 0xFF; // Undefined register.
	}

	u32 TerminalDevice::read(Address offset)
	{
		if (offset == BytesAvailableRegister)
			return static_cast<u32>(availableBytes());
		if (offset == BlockReadCountRegister)
			return _blockReadCount;
		if (offset == DroppedInputRegister)
			return static_cast<u32>(_droppedInputBytes.load(std::memory_order_relaxed));
		if (offset == ModeRegister)
			return _modeGranted.load(std::memory_order_acquire);
		return readStatusOrInput(offset);
	}

	void TerminalDevice::write(Address offset, u32 value)
	{
		// One byte a write: the low one (plan/v2 SPEC 8.1). A whole buffer goes through the block registers.
		if (offset == OutputRegister)
		{
			emitByte(static_cast<u8>(value & 0xFF));
			return;
		}
		if (offset == ErrorOutputRegister)
		{
			emitErrorByte(static_cast<u8>(value & 0xFF));
			return;
		}

		if (offset == ModeRegister)
		{
			const u32 requested = value & ModeRaw;
			_modeRequested.store(requested, std::memory_order_release);
			_modeGranted.store(_modeHandler ? _modeHandler(requested) : 0u, std::memory_order_release);
			return;
		}

		// The block trio: two plain registers latched here, and a command that fires the transfer.
		if (offset == BlockAddressRegister) { _blockAddress = value; return; }
		if (offset == BlockLengthRegister) { _blockLength = value; return; }
		if (offset == BlockCommandRegister)
		{
			if (value == BlockCommandRead)
				blockRead(Address(_blockAddress), _blockLength);
			else if (value == BlockCommandWrite)
				blockWrite(Address(_blockAddress), _blockLength);
			else if (value == BlockCommandWriteError)
				blockWrite(Address(_blockAddress), _blockLength, true);
		}
	}

	const RegisterMap& TerminalDevice::registers() const
	{
		static constexpr RegisterMap map{ "terminal", Registers };
		return map;
	}
}

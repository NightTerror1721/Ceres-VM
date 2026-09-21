#pragma once

#include <ceres/vm/mmio_bus.h>
#include <print>
#include <atomic>
#include <span>
#include <functional>
#include <chrono>
#include <mutex>

namespace ceres::devices
{
	using namespace vm;

	// Every device below follows the same register layout convention: scalar registers sit at low,
	// word-aligned offsets (0x00, 0x04, 0x08, ...) within the device's 64 KiB MMIO slot - the direct
	// replacement for what used to be a handful of single-byte port numbers, just with room to
	// spare. A device that also moves blocks of memory (the disk, the framebuffer) additionally
	// claims three registers near the top of its slot - BLOCK_ADDR/BLOCK_LEN/BLOCK_CMD at
	// 0xF0/0xF4/0xF8 - so a bulk transfer is still one MMIO write to trigger, the same shape `outm`/
	// `inm` used to give it, just addressed like everything else now.

	class SystemControlDevice : public IODevice
	{
	public:
		using ShutdownCallback = std::function<void()>;
		using ResetCallback = std::function<void()>;
		// Told the new value of the features register whenever a program writes it, so the host can
		// pass on to the engine the settings the engine itself has to act on.
		using FeaturesCallback = std::function<void(u32)>;

		// Write-only: writing specific commands to this register triggers system control actions.
		// The low byte is the command; the next byte is the exit status a shutdown reports.
		static inline constexpr Address CommandRegister = Address(0x00);
		// Read-only: how many bytes of RAM the machine has.
		static inline constexpr Address MemorySizeRegister = Address(0x04);
		// Read/write: switches for behaviour that is off by default, so a program that never asks
		// keeps the machine it always had.
		static inline constexpr Address FeaturesRegister = Address(0x08);

		static inline constexpr u32 CommandShutdown = 0x01;
		static inline constexpr u32 CommandReset = 0x02;

		// Division by zero raises the DivisionByZero interrupt (number 4) instead of only setting
		// the Trap flag. The handler returns to the instruction after the division, whose
		// destination was left as it was.
		static inline constexpr u32 FeatureDivisionFault = 1u << 0;

	private:
		ShutdownCallback _shutdownCallback;
		ResetCallback _resetCallback;
		FeaturesCallback _featuresCallback;
		u32 _features = 0;
		std::atomic<u8> _exitCode{ 0 };

	public:
		explicit SystemControlDevice(ShutdownCallback shutdownCallback = {}, ResetCallback resetCallback = {}) :
			_shutdownCallback(std::move(shutdownCallback)),
			_resetCallback(std::move(resetCallback))
		{}

		SystemControlDevice(const SystemControlDevice&) = delete;
		SystemControlDevice(SystemControlDevice&&) = delete;
		~SystemControlDevice() override = default;

		SystemControlDevice& operator=(const SystemControlDevice&) = delete;
		SystemControlDevice& operator=(SystemControlDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::SystemControl, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::SystemControl);
		}

		void setShutdownCallback(ShutdownCallback callback)
		{
			_shutdownCallback = std::move(callback);
		}

		void setResetCallback(ResetCallback callback)
		{
			_resetCallback = std::move(callback);
		}

		void setFeaturesCallback(FeaturesCallback callback)
		{
			_featuresCallback = std::move(callback);
		}

		// The status the program shut the machine down with: the second byte of the word it wrote
		// (0 for a plain byte write, which is what every program written before this existed does).
		u8 exitCode() const noexcept { return _exitCode.load(std::memory_order_relaxed); }

		u32 features() const noexcept { return _features; }

	private:
		// A byte write carries only the command; a halfword or a word carries the exit status
		// above it.
		void command(u32 value)
		{
			const u32 code = value & 0xFF;
			if (code == CommandShutdown)
			{
				_exitCode.store(static_cast<u8>((value >> 8) & 0xFF), std::memory_order_relaxed);
				if (_shutdownCallback)
					_shutdownCallback();
			}
			else if (code == CommandReset)
			{
				if (_resetCallback)
					_resetCallback();
			}
		}

		bool readable(Address offset, u32& value) const
		{
			if (offset == MemorySizeRegister)
			{
				value = static_cast<u32>(memory().size());
				return true;
			}
			if (offset == FeaturesRegister)
			{
				value = _features;
				return true;
			}
			return false;
		}

	public:
		u8 readUnsignedByte(Address offset) override { u32 v; return readable(offset, v) ? static_cast<u8>(v) : 0xFF; }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { u32 v; return readable(offset, v) ? static_cast<u16>(v) : 0xFFFF; }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }
		u32 readUnsignedWord(Address offset) override { u32 v; return readable(offset, v) ? v : 0xFFFFFFFF; }

		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }
		void writeWord(Address offset, u32 value) override
		{
			if (offset == CommandRegister)
			{
				command(value);
			}
			else if (offset == FeaturesRegister)
			{
				_features = value;
				if (_featuresCallback)
					_featuresCallback(value);
			}
		}
	};

	// Gives the machine a sense of time, and with it the asynchronous interrupt source it never
	// had. Until now HALT suspended the machine for good, because nothing could ever wake it.
	//
	// Time is counted in executed instructions rather than wall clock, so a program behaves the
	// same on every run and on every machine. RTC_TIME is the one exception: it reports real
	// seconds, and nothing depends on it.
	class TimerDevice : public IODevice
	{
	public:
		static inline constexpr Address TicksRegister = Address(0x00);   // Read: instructions executed so far
		static inline constexpr Address ClockRegister = Address(0x04);   // Read: seconds since the epoch
		static inline constexpr Address CommandRegister = Address(0x08); // Write: fire after N ticks, 0 disarms
		static inline constexpr Address MillisRegister = Address(0x0C);  // Read: milliseconds since the machine started (wraps every 49 days)

		// Which interrupt the timer requests when it expires. The first user interrupt, so it needs
		// STI to be delivered and cannot surprise a program that never asked for it.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt0;

		// Everything the timer remembers. Exposed so a debugger can put the whole machine back
		// where it was: without the timer, a restored snapshot would keep counting from wherever
		// the live run had got to and fire its interrupt at the wrong moment.
		struct State
		{
			u64 ticks = 0;
			u64 remaining = 0;
			bool periodic = false;
			u64 period = 0;
		};

		// Where the real-time clock register gets its answer. The default is the host's wall clock,
		// which is the one thing in this machine that is not deterministic - so a debugger that
		// replays execution replaces it with a recording.
		using ClockSource = std::function<u32()>;

	private:
		u64 _ticks = 0;
		u64 _remaining = 0;   // 0 means disarmed
		bool _periodic = false;
		u64 _period = 0;
		ClockSource _clockSource;
		ClockSource _millisSource;
		std::chrono::steady_clock::time_point _started = std::chrono::steady_clock::now();

	public:
		TimerDevice() = default;
		TimerDevice(const TimerDevice&) = delete;
		TimerDevice(TimerDevice&&) = delete;
		~TimerDevice() override = default;

		TimerDevice& operator=(const TimerDevice&) = delete;
		TimerDevice& operator=(TimerDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Timer, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Timer);
		}

		u64 ticks() const noexcept { return _ticks; }
		bool isArmed() const noexcept { return _remaining > 0; }

		// Arms the timer directly, for a host that wants a heartbeat without the program asking.
		void arm(u64 ticksFromNow, bool periodic = false) noexcept
		{
			_remaining = ticksFromNow;
			_periodic = periodic;
			_period = ticksFromNow;
		}

		State captureState() const noexcept { return State{ _ticks, _remaining, _periodic, _period }; }

		void restoreState(const State& state) noexcept
		{
			_ticks = state.ticks;
			_remaining = state.remaining;
			_periodic = state.periodic;
			_period = state.period;
		}

		void setClockSource(ClockSource source) { _clockSource = std::move(source); }
		void clearClockSource() { _clockSource = nullptr; }

		// The millisecond counter reads the host's steady clock, which makes it as non-deterministic
		// as the seconds register, and a debugger replaces it in the same way.
		void setMillisSource(ClockSource source) { _millisSource = std::move(source); }
		void clearMillisSource() { _millisSource = nullptr; }

	public:
		// The one device every program that arms it relies on advancing every instruction, whether
		// or not it is armed - TicksRegister reads instructions executed even while disarmed.
		bool needsTick() const noexcept override { return true; }

		void tick() override
		{
			++_ticks;

			if (_remaining == 0)
				return;

			if (--_remaining == 0)
			{
				raiseInterrupt(Interrupt);
				if (_periodic)
					_remaining = _period;
			}
		}

	public:
		u32 readUnsignedWord(Address offset) override
		{
			if (offset == TicksRegister)
				return static_cast<u32>(_ticks);

			if (offset == ClockRegister)
			{
				if (_clockSource)
					return _clockSource();
				return static_cast<u32>(std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::system_clock::now().time_since_epoch()).count());
			}

			if (offset == MillisRegister)
			{
				if (_millisSource)
					return _millisSource();
				return static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - _started).count());
			}

			return 0xFFFFFFFF;
		}

		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		// Writing N to the command register fires the timer N instructions later. Writing 0 disarms
		// it. The high bit asks for a periodic timer that re-arms itself after each expiry.
		void writeWord(Address offset, u32 value) override
		{
			if (offset != CommandRegister)
				return;

			const bool periodic = (value & 0x80000000u) != 0;
			const u64 count = value & 0x7FFFFFFFu;

			arm(count, periodic && count > 0);
		}

		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }
	};

	class TerminalDevice : public IODevice
	{
	public:
		static inline constexpr Address StatusRegister = Address(0x00); // Read-only: bit 0 input available, bit 1 ready for output, bit 2 end of input.
		static inline constexpr Address OutputRegister = Address(0x04); // Write-only: writing a byte to this register outputs it to the terminal.
		static inline constexpr Address InputRegister = Address(0x08);  // Read-only: reading from this register returns the next byte of input, or 0 if none is available.
		static inline constexpr Address BytesAvailableRegister = Address(0x0C); // Read-only: bytes currently sitting unread in the input ring.
		static inline constexpr Address BlockReadCountRegister = Address(0x10); // Read-only: bytes the most recent block-read actually moved into RAM.
		static inline constexpr Address DroppedInputRegister = Address(0x14); // Read-only: input bytes discarded by a full ring (truncated to 32 bits).
		static inline constexpr Address ModeRegister = Address(0x18); // Read/write: write ModeRaw to ask the host for keys as they are pressed (no line editing, no echo); read what the host granted.

		// A bulk transfer: write the RAM address and length, then a command (1 = read from the
		// terminal's input ring into RAM, 2 = write RAM out to the terminal) - the direct
		// replacement for what `inm`/`outm` used to do in one instruction.
		static inline constexpr Address BlockAddressRegister = Address(0xF0);
		static inline constexpr Address BlockLengthRegister = Address(0xF4);
		static inline constexpr Address BlockCommandRegister = Address(0xF8);
		static inline constexpr u32 BlockCommandRead = 1;
		static inline constexpr u32 BlockCommandWrite = 2;

		// Which interrupt pushInput() requests once new bytes are actually sitting in the buffer.
		// The second user interrupt (the first, UserInterrupt0, is the timer's) - so a program that
		// never expects terminal input keeps working exactly as before: STI is still required, and
		// nothing raises this unless pushInput() is called at all.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt1;

	public:
		// Kept small to model a simple UART. Hosts can detect loss through droppedInputBytes().
		static inline constexpr usize InputBufferCapacity = 64;

	private:
		static inline constexpr u8 RxReadyMask = 0x01; // Bit 0 indicates if input is available.
		static inline constexpr u8 TxReadyMask = 0x02; // Bit 1 indicates if the terminal is ready to accept output (always ready in this simple implementation).
		static inline constexpr u8 EofMask = 0x04;     // Bit 2: the host closed the input and every byte it sent has been read - nothing more will ever arrive.

	public:
		// The bits of StatusRegister, for the code that reads them.
		static inline constexpr u32 StatusInputAvailable = RxReadyMask;
		static inline constexpr u32 StatusOutputReady = TxReadyMask;
		static inline constexpr u32 StatusEndOfInput = EofMask;

		// The bits of ModeRegister. A write is a request; a read is the answer. A host that cannot give
		// raw keys (input from a pipe, a file) answers 0, and the program keeps reading the terminal.
		static inline constexpr u32 ModeRaw = 1u << 0;        // Keys arrive as they are pressed: no line buffering, no echo.
		static inline constexpr u32 ModeKeystrokes = 1u << 1; // Read-only: they arrive on the keyboard device's KeyRegister.

		// Decides what a write to ModeRegister gets: it is handed the requested bits and returns the granted
		// ones. The host switches its console in there. Empty means nothing is ever granted.
		using ModeHandler = std::function<u32(u32)>;

	private:

	public:
		// Where a byte written to the output register ends up. `ceres run` leaves it empty and the
		// bytes go to stdout, which is what a plain terminal program wants; a debugger installs
		// one so the program's output can be forwarded to the editor instead of racing with a
		// protocol sharing that same stream.
		using OutputSink = std::function<void(u8)>;

	private:
		std::array<u8, InputBufferCapacity> _buffer{};
		std::atomic<usize> _head{0};
		std::atomic<usize> _tail{0};
		std::atomic<u64> _droppedInputBytes{0};
		std::atomic<bool> _inputClosed{false};
		// Protects the byte array while a debugger snapshots it. Head/tail remain atomic so the
		// single-producer/single-consumer fast path still has a minimal synchronization surface.
		mutable std::mutex _inputMutex;
		OutputSink _outputSink;
		u32 _blockAddress = 0;
		u32 _blockLength = 0;
		u32 _blockReadCount = 0;
		ModeHandler _modeHandler;
		std::atomic<u32> _modeRequested{0};
		std::atomic<u32> _modeGranted{0};

	public:
		TerminalDevice() = default;
		TerminalDevice(const TerminalDevice&) = delete;
		TerminalDevice(TerminalDevice&&) = delete;
		~TerminalDevice() override = default;

		TerminalDevice& operator=(const TerminalDevice&) = delete;
		TerminalDevice& operator=(TerminalDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Terminal, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Terminal);
		}

		void pushInput(std::span<const u8> input)
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

		void pushInput(std::string_view input)
		{
			pushInput(std::span<const u8>(reinterpret_cast<const u8*>(input.data()), input.size()));
		}

		void pushInput(const char* input)
		{
			pushInput(std::string_view(input));
		}

		void pushInput(char input)
		{
			pushInput(std::string_view(&input, 1));
		}

		// The host has nothing more to send: stdin was closed, or the pipe ran dry. The status
		// register reports end of input once the program has also read what is still buffered, so a
		// reader can tell "no data yet" from "no data ever". The interrupt is raised so a program
		// halted waiting for input wakes up to notice.
		void closeInput()
		{
			_inputClosed.store(true, std::memory_order_release);
			raiseInterrupt(Interrupt);
		}

		bool isInputClosed() const noexcept { return _inputClosed.load(std::memory_order_acquire); }

		u64 droppedInputBytes() const noexcept { return _droppedInputBytes.load(std::memory_order_relaxed); }

		// How many bytes are currently buffered and unread. The one number a program needs to
		// decide whether to block-read, and how large a block to ask for, without polling the
		// status bit and guessing.
		usize availableBytes() const noexcept
		{
			const usize head = _head.load(std::memory_order_acquire);
			const usize tail = _tail.load(std::memory_order_acquire);
			return (tail - head + InputBufferCapacity) % InputBufferCapacity;
		}

		void setModeHandler(ModeHandler handler) { _modeHandler = std::move(handler); }

		// Whether the program asked for raw keys, so a host can tell whether to also type into the terminal.
		bool rawRequested() const noexcept { return (_modeRequested.load(std::memory_order_acquire) & ModeRaw) != 0; }

		void setOutputSink(OutputSink sink) { _outputSink = std::move(sink); }
		void clearOutputSink() { _outputSink = nullptr; }

		// The input ring, so a debugger restoring a snapshot can put back exactly the bytes the
		// program had not yet read. Copied rather than shared: the live buffer is written from
		// another thread.
		struct State
		{
			std::array<u8, InputBufferCapacity> buffer{};
			usize head = 0;
			usize tail = 0;
			bool closed = false;
		};

		State captureState() const noexcept
		{
			const std::lock_guard lock{_inputMutex};
			State state;
			state.buffer = _buffer;
			state.head = _head.load(std::memory_order_acquire);
			state.tail = _tail.load(std::memory_order_acquire);
			state.closed = _inputClosed.load(std::memory_order_acquire);
			return state;
		}

		void restoreState(const State& state) noexcept
		{
			const std::lock_guard lock{_inputMutex};
			_buffer = state.buffer;
			_head.store(state.head, std::memory_order_release);
			_tail.store(state.tail, std::memory_order_release);
			_inputClosed.store(state.closed, std::memory_order_release);
		}

	private:
		// The single place output leaves the device. Bytes are handed over one at a time and
		// deliberately not decoded here: a multi-byte UTF-8 sequence is written by the program as
		// several separate register writes, so only the consumer knows where a character ends.
		void emitByte(u8 value)
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

		void blockRead(Address ramAddress, u32 size)
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

		void blockWrite(Address ramAddress, u32 size)
		{
			if (size == 0)
				return;

			const u32 clampSize = memory().clampBlockSize(ramAddress, size);
			if (clampSize == 0)
				return;

			const auto buffer = memory().peekBytes(ramAddress, clampSize);
			for (u32 i = 0; i < buffer.size(); ++i)
				emitByte(buffer[i]);
		}

	public:
		u8 readUnsignedByte(Address offset) override
		{
			if (offset == StatusRegister)
			{
				u8 status = 0;
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

				u8 value = _buffer[currentHead];
				_head.store((currentHead + 1) % InputBufferCapacity, std::memory_order_release);
				return value;
			}

			return 0xFF; // Undefined register.
		}
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedByte(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		u32 readUnsignedWord(Address offset) override
		{
			if (offset == BytesAvailableRegister)
				return static_cast<u32>(availableBytes());
			if (offset == BlockReadCountRegister)
				return _blockReadCount;
			if (offset == DroppedInputRegister)
				return static_cast<u32>(_droppedInputBytes.load(std::memory_order_relaxed));
			if (offset == ModeRegister)
				return _modeGranted.load(std::memory_order_acquire);
			return static_cast<u32>(readUnsignedByte(offset));
		}

		void writeByte(Address offset, u8 value) override
		{
			if (offset == OutputRegister)
				emitByte(value);
		}
		void writeHalfword(Address offset, u16 value) override
		{
			if (offset == OutputRegister)
			{
				emitByte(static_cast<u8>(value & 0xFF)); // Output the lower byte as a character.
				emitByte(static_cast<u8>((value >> 8) & 0xFF)); // Output the upper byte as a character.
			}
		}
		void writeWord(Address offset, u32 value) override
		{
			if (offset == OutputRegister)
			{
				emitByte(static_cast<u8>(value & 0xFF));
				emitByte(static_cast<u8>((value >> 8) & 0xFF));
				emitByte(static_cast<u8>((value >> 16) & 0xFF));
				emitByte(static_cast<u8>((value >> 24) & 0xFF));
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
			}
		}
	};

	// A real DMA engine, not the pseudo-DMA the port opcodes used to be: SRC/DST/LEN/CMD are
	// ordinary registers, and completion is a tick later rather than instantaneous - modelled the
	// same way TimerDevice already models a delay, so a program can either poll STATUS or wait for
	// the interrupt. It moves memory the VM already knows how to move
	// (Memory::copyBytesUnchecked): RAM to RAM today, and RAM to or from a device's own MMIO window
	// once a device chooses to expose one, since both are just addresses in the same space.
	class DmaController : public IODevice
	{
	public:
		static inline constexpr Address SourceRegister = Address(0x00);      // Write: source physical address
		static inline constexpr Address DestinationRegister = Address(0x04); // Write: destination physical address
		static inline constexpr Address LengthRegister = Address(0x08);      // Write: bytes to move
		static inline constexpr Address CommandRegister = Address(0x0C);     // Write: 1 starts the transfer latched above
		static inline constexpr Address StatusRegister = Address(0x10);      // Read: Busy / Done bits
		static inline constexpr Address TransferredRegister = Address(0x14); // Read: bytes the last completed transfer actually moved

		static inline constexpr u32 CommandStart = 1;
		static inline constexpr u32 StatusBusy = 1u << 0;
		static inline constexpr u32 StatusDone = 1u << 1;

		// Third user interrupt: UserInterrupt0 is the timer's, UserInterrupt1 the terminal's.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt2;

	private:
		u32 _source = 0;
		u32 _destination = 0;
		u32 _length = 0;
		u32 _status = 0;
		u32 _transferred = 0;
		bool _pending = false;

	public:
		DmaController() = default;
		DmaController(const DmaController&) = delete;
		DmaController(DmaController&&) = delete;
		~DmaController() override = default;

		DmaController& operator=(const DmaController&) = delete;
		DmaController& operator=(DmaController&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Dma, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Dma);
		}

	public:
		// Must be unconditionally true, not "return _pending": MmioBus only re-reads needsTick() when
		// the topology changes (attach/detach), not every instruction, so a device that flipped this
		// on the fly could arm a transfer that then never sees the tick() that lands it.
		bool needsTick() const noexcept override { return true; }

		// Arms on the CMD write; the actual copy happens on the next tick(), one instruction later -
		// never on the same step that requested it, so a program relying on the interrupt (rather
		// than busy-polling STATUS) always sees a real handoff instead of an already-finished copy.
		void tick() override
		{
			if (!_pending)
				return;

			// A SRC/DST/LEN that runs past the end of memory is clamped rather than fatal: the
			// copy moves what fits, and TransferredRegister reports exactly how much.
			const u32 effective = std::min(
				memory().clampBlockSizeUnchecked(Address(_source), _length),
				memory().clampBlockSizeUnchecked(Address(_destination), _length));

			memory().copyBytesUnchecked(Address(_source), Address(_destination), effective);
			// RAM to RAM always moves the whole length, but a source device that yields fewer
			// bytes (a short terminal read) would land a smaller number here - which is exactly
			// what this register exists to report.
			_transferred = effective;
			_pending = false;
			_status = StatusDone;
			raiseInterrupt(Interrupt);
		}

	public:
		u32 readUnsignedWord(Address offset) override
		{
			if (offset == StatusRegister)
				return _status;
			if (offset == TransferredRegister)
				return _transferred;
			return 0;
		}
		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		void writeWord(Address offset, u32 value) override
		{
			if (offset == SourceRegister) { _source = value; return; }
			if (offset == DestinationRegister) { _destination = value; return; }
			if (offset == LengthRegister) { _length = value; return; }
			if (offset == CommandRegister && value == CommandStart)
			{
				_pending = true;
				_status = StatusBusy;
			}
		}
		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }
	};
}

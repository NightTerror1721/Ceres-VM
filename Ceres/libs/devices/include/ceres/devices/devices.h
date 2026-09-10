#pragma once

#include <ceres/vm/mmio_bus.h>
#include <print>
#include <atomic>
#include <span>
#include <functional>
#include <chrono>

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

		// Write-only: writing specific commands to this register triggers system control actions.
		static inline constexpr Address CommandRegister = Address(0x00);

	private:
		ShutdownCallback _shutdownCallback;
		ResetCallback _resetCallback;

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

	public:
		u8 readUnsignedByte(Address) override { return 0xFF; /* No readable registers. */ }
		i8 readSignedByte(Address) override { return -1; }
		u16 readUnsignedHalfword(Address) override { return 0xFFFF; }
		i16 readSignedHalfword(Address) override { return -1; }
		u32 readUnsignedWord(Address) override { return 0xFFFFFFFF; }

		void writeByte(Address offset, u8 value) override
		{
			if (offset != CommandRegister)
				return;

			if (value == 0x01) // Shutdown command
			{
				if (_shutdownCallback)
					_shutdownCallback();
			}
			else if (value == 0x02) // Reset command
			{
				if (_resetCallback)
					_resetCallback();
			}
		}
		void writeHalfword(Address offset, u16 value) override { writeByte(offset, static_cast<u8>(value)); }
		void writeWord(Address offset, u32 value) override { writeByte(offset, static_cast<u8>(value)); }
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

	public:
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
		static inline constexpr Address StatusRegister = Address(0x00); // Read-only: 0x01 if input is available, 0x00 otherwise.
		static inline constexpr Address OutputRegister = Address(0x04); // Write-only: writing a byte to this register outputs it to the terminal.
		static inline constexpr Address InputRegister = Address(0x08);  // Read-only: reading from this register returns the next byte of input, or 0 if none is available.

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

	private:
		static inline constexpr u8 RxReadyMask = 0x01; // Bit 0 indicates if input is available.
		static inline constexpr u8 TxReadyMask = 0x02; // Bit 1 indicates if the terminal is ready to accept output (always ready in this simple implementation).
		static inline constexpr usize MaxInputBufferSize = 64; // Maximum size of the input buffer.

	public:
		// Where a byte written to the output register ends up. `ceres run` leaves it empty and the
		// bytes go to stdout, which is what a plain terminal program wants; a debugger installs
		// one so the program's output can be forwarded to the editor instead of racing with a
		// protocol sharing that same stream.
		using OutputSink = std::function<void(u8)>;

	private:
		std::array<u8, MaxInputBufferSize> _buffer{};
		std::atomic<usize> _head{0};
		std::atomic<usize> _tail{0};
		OutputSink _outputSink;
		u32 _blockAddress = 0;
		u32 _blockLength = 0;

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
			bool wroteAnyByte = false;
			for (u8 byte : input)
			{
				usize nextTail = (_tail.load(std::memory_order_relaxed) + 1) % MaxInputBufferSize;
				if (nextTail == _head.load(std::memory_order_acquire))
					continue;

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

		void setOutputSink(OutputSink sink) { _outputSink = std::move(sink); }
		void clearOutputSink() { _outputSink = nullptr; }

		// The input ring, so a debugger restoring a snapshot can put back exactly the bytes the
		// program had not yet read. Copied rather than shared: the live buffer is written from
		// another thread.
		struct State
		{
			std::array<u8, MaxInputBufferSize> buffer{};
			usize head = 0;
			usize tail = 0;
		};

		State captureState() const noexcept
		{
			State state;
			state.buffer = _buffer;
			state.head = _head.load(std::memory_order_acquire);
			state.tail = _tail.load(std::memory_order_acquire);
			return state;
		}

		void restoreState(const State& state) noexcept
		{
			_buffer = state.buffer;
			_head.store(state.head, std::memory_order_release);
			_tail.store(state.tail, std::memory_order_release);
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
				return;

			auto buffer = memory().peekMutBytes(ramAddress, size);
			usize bytesRead = 0;
			while (bytesRead < buffer.size())
			{
				usize currentHead = _head.load(std::memory_order_relaxed);
				if (currentHead == _tail.load(std::memory_order_acquire))
					break; // No more input available.

				buffer[bytesRead] = _buffer[currentHead];
				_head.store((currentHead + 1) % MaxInputBufferSize, std::memory_order_release);
				++bytesRead;
			}
		}

		void blockWrite(Address ramAddress, u32 size)
		{
			if (size == 0)
				return;

			const auto buffer = memory().peekBytes(ramAddress, size);
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
				status |= TxReadyMask; // Terminal is always ready to accept output.
				return status;
			}

			if (offset == InputRegister)
			{
				usize currentHead = _head.load(std::memory_order_relaxed);
				if (currentHead == _tail.load(std::memory_order_acquire))
					return 0; // No input available, return 0.

				u8 value = _buffer[currentHead];
				_head.store((currentHead + 1) % MaxInputBufferSize, std::memory_order_release);
				return value;
			}

			return 0xFF; // Undefined register.
		}
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedByte(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }
		u32 readUnsignedWord(Address offset) override { return static_cast<u32>(readUnsignedHalfword(offset)); }

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
		// Arms on the CMD write; the actual copy happens on the next tick(), one instruction later -
		// never on the same step that requested it, so a program relying on the interrupt (rather
		// than busy-polling STATUS) always sees a real handoff instead of an already-finished copy.
		void tick() override
		{
			if (!_pending)
				return;

			memory().copyBytesUnchecked(Address(_source), Address(_destination), _length);
			_pending = false;
			_status = StatusDone;
			raiseInterrupt(Interrupt);
		}

	public:
		u32 readUnsignedWord(Address offset) override
		{
			if (offset == StatusRegister)
				return _status;
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

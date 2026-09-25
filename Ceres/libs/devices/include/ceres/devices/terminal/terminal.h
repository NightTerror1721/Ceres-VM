#pragma once

// The terminal: the program's standard input, output and error streams.

#include <ceres/vm/mmio_bus.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <print>
#include <span>

namespace ceres::devices
{
	using namespace vm;

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
		static inline constexpr Address ErrorOutputRegister = Address(0x1C); // Write-only: a byte for the error stream - the host's stderr under `ceres run`, kept apart from the output.

		// A bulk transfer: write the RAM address and length, then a command (1 = read from the
		// terminal's input ring into RAM, 2 = write RAM out to the terminal) - the direct
		// replacement for what `inm`/`outm` used to do in one instruction.
		static inline constexpr Address BlockAddressRegister = Address(0xF0);
		static inline constexpr Address BlockLengthRegister = Address(0xF4);
		static inline constexpr Address BlockCommandRegister = Address(0xF8);
		static inline constexpr u32 BlockCommandRead = 1;
		static inline constexpr u32 BlockCommandWrite = 2;
		static inline constexpr u32 BlockCommandWriteError = 3;   // RAM out to the error stream

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
		OutputSink _errorSink;
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

		void pushInput(std::span<const u8> input);

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
		void closeInput();

		bool isInputClosed() const noexcept { return _inputClosed.load(std::memory_order_acquire); }

		u64 droppedInputBytes() const noexcept { return _droppedInputBytes.load(std::memory_order_relaxed); }

		// How many bytes are currently buffered and unread. The one number a program needs to
		// decide whether to block-read, and how large a block to ask for, without polling the
		// status bit and guessing.
		usize availableBytes() const noexcept;

		void setModeHandler(ModeHandler handler) { _modeHandler = std::move(handler); }

		// Whether the program asked for raw keys, so a host can tell whether to also type into the terminal.
		bool rawRequested() const noexcept { return (_modeRequested.load(std::memory_order_acquire) & ModeRaw) != 0; }

		void setOutputSink(OutputSink sink) { _outputSink = std::move(sink); }
		// Where a byte of the error stream goes; empty means the host's stderr.
		void setErrorSink(OutputSink sink) { _errorSink = std::move(sink); }
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

		State captureState() const noexcept;

		void restoreState(const State& state) noexcept;

	private:
		// The single place output leaves the device. Bytes are handed over one at a time and
		// deliberately not decoded here: a multi-byte UTF-8 sequence is written by the program as
		// several separate register writes, so only the consumer knows where a character ends.
		void emitByte(u8 value);

		void emitErrorByte(u8 value);

		void blockRead(Address ramAddress, u32 size);

		void blockWrite(Address ramAddress, u32 size, bool error = false);

	public:
		u8 readUnsignedByte(Address offset) override;
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedByte(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		u32 readUnsignedWord(Address offset) override;

		void writeByte(Address offset, u8 value) override;
		void writeHalfword(Address offset, u16 value) override;
		void writeWord(Address offset, u32 value) override;
	};
}

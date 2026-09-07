#pragma once

#include "io_ports.h"
#include <print>
#include <atomic>
#include <span>
#include <functional>
#include <chrono>

namespace ceres::vm
{
	class SystemControlDevice : public IODevice
	{
	public:
		using ShutdownCallback = std::function<void()>;
		using ResetCallback = std::function<void()>;

		// Write-only: Writing specific commands to this port triggers system control actions (e.g., shutdown).
		static inline constexpr PortNumber SystemControlPort = default_ports::SYS_CONTROL;

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
		void attachTo(IOPorts& ioPorts)
		{
			ioPorts.attach(SystemControlPort, *this);
		}

		void detachFrom(IOPorts& ioPorts)
		{
			ioPorts.detach(SystemControlPort);
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
		u8 readPortUnsignedByte(PortNumber port) override { return 0xFF; /* No readable ports, return 0xFF for all ports. */ }
		i8 readPortSignedByte(PortNumber port) override { return -1; /* No readable ports, return -1 for all ports. */ }
		u16 readPortUnsignedHalfword(PortNumber port) override { return 0xFFFF; /* No readable ports, return 0xFFFF for all ports. */ }
		i16 readPortSignedHalfword(PortNumber port) override { return -1; /* No readable ports, return -1 for all ports. */ }
		u32 readPortUnsignedWord(PortNumber port) override { return 0xFFFFFFFF; /* No readable ports, return 0xFFFFFFFF for all ports. */ }
		void readPort(PortNumber port, Address address, u32 size) override
		{
			memory().setBytes(address, 0xFF, size); // No readable ports, fill memory with 0xFF.
		}

		void writePortByte(PortNumber port, u8 value) override
		{
			if (port == SystemControlPort)
			{
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
		}
		void writePortHalfword(PortNumber port, u16 value) override { writePortByte(port, static_cast<u8>(value)); }
		void writePortWord(PortNumber port, u32 value) override { writePortByte(port, static_cast<u8>(value)); }
		void writePort(PortNumber port, Address address, u32 size) override
		{
			if (size > 0)
			{
				auto bytes = memory().peekBytes(address, size);
				for (u32 i = 0; i < bytes.size(); ++i)
					writePortByte(port, bytes[i]);
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
		static inline constexpr PortNumber TicksPort = default_ports::SYS_TICKS;   // Read: instructions executed so far
		static inline constexpr PortNumber ClockPort = default_ports::RTC_TIME;   // Read: seconds since the epoch
		static inline constexpr PortNumber CommandPort = default_ports::TIMER_CMD; // Write: fire after N ticks, 0 disarms

		// Which interrupt the timer requests when it expires. The first user interrupt, so it needs
		// STI to be delivered and cannot surprise a program that never asked for it.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt0;

	private:
		u64 _ticks = 0;
		u64 _remaining = 0;   // 0 means disarmed
		bool _periodic = false;
		u64 _period = 0;

	public:
		TimerDevice() = default;
		TimerDevice(const TimerDevice&) = delete;
		TimerDevice(TimerDevice&&) = delete;
		~TimerDevice() override = default;

		TimerDevice& operator=(const TimerDevice&) = delete;
		TimerDevice& operator=(TimerDevice&&) = delete;

	public:
		void attachTo(IOPorts& ioPorts)
		{
			ioPorts.attach(TicksPort, *this);
			ioPorts.attach(ClockPort, *this);
			ioPorts.attach(CommandPort, *this);
		}

		void detachFrom(IOPorts& ioPorts)
		{
			ioPorts.detach(TicksPort);
			ioPorts.detach(ClockPort);
			ioPorts.detach(CommandPort);
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
		u32 readPortUnsignedWord(PortNumber port) override
		{
			switch (port)
			{
				case TicksPort:
					return static_cast<u32>(_ticks);

				case ClockPort:
					return static_cast<u32>(std::chrono::duration_cast<std::chrono::seconds>(
						std::chrono::system_clock::now().time_since_epoch()).count());

				default:
					return 0xFFFFFFFF;
			}
		}

		u8 readPortUnsignedByte(PortNumber port) override { return static_cast<u8>(readPortUnsignedWord(port)); }
		i8 readPortSignedByte(PortNumber port) override { return static_cast<i8>(readPortUnsignedByte(port)); }
		u16 readPortUnsignedHalfword(PortNumber port) override { return static_cast<u16>(readPortUnsignedWord(port)); }
		i16 readPortSignedHalfword(PortNumber port) override { return static_cast<i16>(readPortUnsignedHalfword(port)); }
		void readPort(PortNumber port, Address address, u32 size) override
		{
			if (size >= sizeof(u32))
				memory().write<u32>(address, readPortUnsignedWord(port));
		}

		// Writing N to the command port fires the timer N instructions later. Writing 0 disarms it.
		// The high bit asks for a periodic timer that re-arms itself after each expiry.
		void writePortWord(PortNumber port, u32 value) override
		{
			if (port != CommandPort)
				return;

			const bool periodic = (value & 0x80000000u) != 0;
			const u64 count = value & 0x7FFFFFFFu;

			arm(count, periodic && count > 0);
		}

		void writePortByte(PortNumber port, u8 value) override { writePortWord(port, value); }
		void writePortHalfword(PortNumber port, u16 value) override { writePortWord(port, value); }
		void writePort(PortNumber port, Address address, u32 size) override
		{
			if (size >= sizeof(u32))
				writePortWord(port, memory().read<u32>(address));
		}
	};

	class TerminalDevice : public IODevice
	{
	public:
		static inline constexpr PortNumber StatusPort = default_ports::TERM_STATUS; // Read-only: 0x01 if input is available, 0x00 otherwise.
		static inline constexpr PortNumber OutputPort = default_ports::TERM_OUT; // Write-only: Writing a byte to this port outputs it to the terminal.
		static inline constexpr PortNumber InputPort = default_ports::TERM_IN;  // Read-only: Reading from this port returns the next byte of input, or 0 if no input is available.

	private:
		static inline constexpr u8 RxReadyMask = 0x01; // Bit 0 indicates if input is available.
		static inline constexpr u8 TxReadyMask = 0x02; // Bit 1 indicates if the terminal is ready to accept output (always ready in this simple implementation).
		static inline constexpr usize MaxInputBufferSize = 64; // Maximum size of the input buffer.

	public:
		// Where a byte written to the output port ends up. `ceres run` leaves it empty and the
		// bytes go to stdout, which is what a plain terminal program wants; a debugger installs
		// one so the program's output can be forwarded to the editor instead of racing with a
		// protocol sharing that same stream.
		using OutputSink = std::function<void(u8)>;

	private:
		std::array<u8, MaxInputBufferSize> _buffer{};
		std::atomic<usize> _head{0};
		std::atomic<usize> _tail{0};
		OutputSink _outputSink;

	public:
		TerminalDevice() = default;
		TerminalDevice(const TerminalDevice&) = delete;
		TerminalDevice(TerminalDevice&&) = delete;
		~TerminalDevice() override = default;

		TerminalDevice& operator=(const TerminalDevice&) = delete;
		TerminalDevice& operator=(TerminalDevice&&) = delete;

	public:
		void attachTo(IOPorts& ioPorts)
		{
			ioPorts.attach(StatusPort, *this);
			ioPorts.attach(OutputPort, *this);
			ioPorts.attach(InputPort, *this);
		}

		void detachFrom(IOPorts& ioPorts)
		{
			ioPorts.detach(StatusPort);
			ioPorts.detach(OutputPort);
			ioPorts.detach(InputPort);
		}

		void pushInput(std::span<const u8> input)
		{
			for (u8 byte : input)
			{
				usize nextTail = (_tail.load(std::memory_order_relaxed) + 1) % MaxInputBufferSize;
				if (nextTail == _head.load(std::memory_order_acquire))
					continue;

				_buffer[_tail.load(std::memory_order_relaxed)] = byte;
				_tail.store(nextTail, std::memory_order_release);
			}
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

	private:
		// The single place output leaves the device. Bytes are handed over one at a time and
		// deliberately not decoded here: a multi-byte UTF-8 sequence is written by the program as
		// several separate port writes, so only the consumer knows where a character ends.
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

	public:
		u8 readPortUnsignedByte(PortNumber port) override
		{
			switch (port)
			{
				case StatusPort:
				{
					u8 status = 0;
					if (_head.load(std::memory_order_acquire) != _tail.load(std::memory_order_acquire))
						status |= RxReadyMask; // Set RxReady if input is available.
					status |= TxReadyMask; // Terminal is always ready to accept output.
					return status;
				}

				case InputPort:
				{
					usize currentHead = _head.load(std::memory_order_relaxed);
					if (currentHead == _tail.load(std::memory_order_acquire))
						return 0; // No input available, return 0.

					u8 value = _buffer[currentHead];
					_head.store((currentHead + 1) % MaxInputBufferSize, std::memory_order_release);
					return value;
				}

				default:
					return 0xFF; // Return 0xFF for undefined ports.
			}
		}
		i8 readPortSignedByte(PortNumber port) override { return static_cast<i8>(readPortUnsignedByte(port)); }
		u16 readPortUnsignedHalfword(PortNumber port) override { return static_cast<u16>(readPortUnsignedByte(port)); }
		i16 readPortSignedHalfword(PortNumber port) override { return static_cast<i16>(readPortUnsignedHalfword(port)); }
		u32 readPortUnsignedWord(PortNumber port) override { return static_cast<u32>(readPortUnsignedHalfword(port)); }
		void readPort(PortNumber port, Address address, u32 size) override
		{
			if (size > 0)
			{
				if (port == InputPort)
				{
					auto buffer = memory().peekMutBytes(address, size);
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
				else if (port == StatusPort)
				{
					auto buffer = memory().peekMutBytes(address, size);
					memory().write<u8>(address, readPortUnsignedByte(StatusPort));

					if (size > 1)
						memory().setBytes(address + 1_addr, 0xFF, size - 1); // Fill the rest with 0xFF for undefined ports.
				}
			}
		}

		void writePortByte(PortNumber port, u8 value) override
		{
			if (port == OutputPort)
			{
				emitByte(value);
			}
		}
		void writePortHalfword(PortNumber port, u16 value) override
		{
			if (port == OutputPort)
			{
				emitByte(static_cast<u8>(value & 0xFF)); // Output the lower byte as a character.
				emitByte(static_cast<u8>((value >> 8) & 0xFF)); // Output the upper byte as a character.
			}
		}
		void writePortWord(PortNumber port, u32 value) override
		{
			if (port == OutputPort)
			{
				emitByte(static_cast<u8>(value & 0xFF)); // Output the lowest byte as a character.
				emitByte(static_cast<u8>((value >> 8) & 0xFF)); // Output the second byte as a character.
				emitByte(static_cast<u8>((value >> 16) & 0xFF)); // Output the third byte as a character.
				emitByte(static_cast<u8>((value >> 24) & 0xFF)); // Output the highest byte as a character.
			}
		}
		void writePort(PortNumber port, Address address, u32 size) override
		{
			if (port == OutputPort && size > 0)
			{
				auto buffer = memory().peekMutBytes(address, size);
				for (u32 i = 0; i < buffer.size(); ++i)
					emitByte(buffer[i]); // Output each byte as a character to the terminal.
			}
		}
	};
}

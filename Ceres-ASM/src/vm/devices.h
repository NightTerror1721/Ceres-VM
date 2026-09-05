#pragma once

#include "io_ports.h"
#include <print>
#include <atomic>
#include <span>
#include <functional>

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

	private:
		std::array<u8, MaxInputBufferSize> _buffer{};
		std::atomic<usize> _head{0};
		std::atomic<usize> _tail{0};

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
				std::print("{:c}", value); // Output the byte as a character to the terminal.
			}
		}
		void writePortHalfword(PortNumber port, u16 value) override
		{
			if (port == OutputPort)
			{
				std::print("{:c}", static_cast<char>(value & 0xFF)); // Output the lower byte as a character.
				std::print("{:c}", static_cast<char>((value >> 8) & 0xFF)); // Output the upper byte as a character.
			}
		}
		void writePortWord(PortNumber port, u32 value) override
		{
			if (port == OutputPort)
			{
				std::print("{:c}", static_cast<char>(value & 0xFF)); // Output the lowest byte as a character.
				std::print("{:c}", static_cast<char>((value >> 8) & 0xFF)); // Output the second byte as a character.
				std::print("{:c}", static_cast<char>((value >> 16) & 0xFF)); // Output the third byte as a character.
				std::print("{:c}", static_cast<char>((value >> 24) & 0xFF)); // Output the highest byte as a character.
			}
		}
		void writePort(PortNumber port, Address address, u32 size) override
		{
			if (port == OutputPort && size > 0)
			{
				auto buffer = memory().peekMutBytes(address, size);
				for (u32 i = 0; i < buffer.size(); ++i)
					std::print("{:c}", buffer[i]); // Output each byte as a character to the terminal.
			}
		}
	};
}

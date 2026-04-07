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
		u8 readPort(PortNumber port) override
		{
			return 0xFF; // No readable ports, return 0xFF for all ports.
		}

		void writePort(PortNumber port, u8 value) override
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
		u8 readPort(PortNumber port) override
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

		void writePort(PortNumber port, u8 value) override
		{
			if (port == OutputPort)
			{
				std::print("{:c}", value); // Output the byte as a character to the terminal.
			}
		}
	};
}

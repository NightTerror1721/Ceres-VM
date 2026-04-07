#pragma once

#include "common/types.h"
#include <array>

namespace ceres::vm
{
	using PortNumber = u8;

	class IODevice
	{
	public:
		virtual ~IODevice() = default;

	public:
		virtual u8 readPort(PortNumber port) = 0;
		virtual void writePort(PortNumber port, u8 value) = 0;
	};

	class DummyDevice final : public IODevice
	{
		u8 readPort(PortNumber port) override { return 0xFF; }
		void writePort(PortNumber port, u8 value) override {}
	};

	class IOPorts
	{
	public:
		static inline constexpr usize MaxPorts = 256;

	private:
		std::array<IODevice*, MaxPorts> _devices{};

	public:
		IOPorts() = default;
		IOPorts(const IOPorts&) = delete;
		IOPorts(IOPorts&&) = delete;
		~IOPorts() = default;

		IOPorts& operator=(const IOPorts&) = delete;
		IOPorts& operator=(IOPorts&&) = delete;

	public:
		inline constexpr void attach(PortNumber port, IODevice& device)
		{
			_devices[port] = &device;
		}

		inline constexpr void attachRange(PortNumber start, PortNumber end, IODevice& device)
		{
			for (PortNumber port = start; port <= end; ++port)
				_devices[port] = &device;
		}

		inline constexpr void detach(PortNumber port)
		{
			_devices[port] = nullptr;
		}

		inline constexpr void detachRange(PortNumber start, PortNumber end)
		{
			for (PortNumber port = start; port <= end; ++port)
				_devices[port] = nullptr;
		}

		inline constexpr bool isAttached(PortNumber port) const
		{
			return _devices[port] != nullptr;
		}

		forceinline u8 read(PortNumber port)
		{
			if (IODevice* device = _devices[port])
				return device->readPort(port);
			return 0xFF; // Default to returning 0xFF if no device is attached
		}

		forceinline void write(PortNumber port, u8 value)
		{
			if (IODevice* device = _devices[port])
				device->writePort(port, value);
		}
	};

	namespace default_ports
	{
		static inline constexpr PortNumber TERM_STATUS = 0x00;
		static inline constexpr PortNumber TERM_OUT = 0x01;
		static inline constexpr PortNumber TERM_IN = 0x02;
		static inline constexpr PortNumber DEBUG_HEX = 0x03;

		static inline constexpr PortNumber SYS_TICKS = 0x10;
		static inline constexpr PortNumber RTC_TIME = 0x11;
		static inline constexpr PortNumber TIMER_CMD = 0x12;

		static inline constexpr PortNumber DISK_STATUS = 0x20;
		static inline constexpr PortNumber DISK_CMD = 0x21;
		static inline constexpr PortNumber DISK_SECTOR = 0x22;
		static inline constexpr PortNumber DISK_DATA = 0x23;

		static inline constexpr PortNumber GPU_CMD = 0x30;
		static inline constexpr PortNumber GPU_WIDTH = 0x31;
		static inline constexpr PortNumber GPU_HEIGHT = 0x32;
		static inline constexpr PortNumber SPRITE_DATA = 0x33;

		static inline constexpr PortNumber MOUSE_STATUS = 0x40;
		static inline constexpr PortNumber MOUSE_X = 0x41;
		static inline constexpr PortNumber MOUSE_Y = 0x42;
		static inline constexpr PortNumber GAMEPAD_STATE = 0x43;

		static inline constexpr PortNumber AUDIO_CMD = 0x50;
		static inline constexpr PortNumber AUDIO_FREQ = 0x51;

		static inline constexpr PortNumber NET_STATUS = 0x60;
		static inline constexpr PortNumber NET_SEND = 0x61;
		static inline constexpr PortNumber NET_RECV = 0x62;

		// Ports 0x70 to 0xEF are reserved for future expansion of default devices. //

		static inline constexpr PortNumber SYS_RNG = 0xFE;
		static inline constexpr PortNumber SYS_CONTROL = 0xFF;
	}
}

#pragma once

#include "common/types.h"
#include "address.h"
#include "memory.h"
#include <array>
#include <limits>

namespace ceres::vm
{
	using PortNumber = u8;

	class IOPorts;

	class IODevice
	{
	private:
		Memory* _memory = nullptr;

	public:
		virtual ~IODevice() = default;

	public:
		virtual u8 readPortUnsignedByte(PortNumber port) = 0;
		virtual i8 readPortSignedByte(PortNumber port) = 0;
		virtual u16 readPortUnsignedHalfword(PortNumber port) = 0;
		virtual i16 readPortSignedHalfword(PortNumber port) = 0;
		virtual u32 readPortUnsignedWord(PortNumber port) = 0;
		virtual void readPort(PortNumber, Address address, u32 size) = 0;

		virtual void writePortByte(PortNumber port, u8 value) = 0;
		virtual void writePortHalfword(PortNumber port, u16 value) = 0;
		virtual void writePortWord(PortNumber port, u32 value) = 0;
		virtual void writePort(PortNumber port, Address address, u32 size) = 0;

	protected:
		Memory& memory() { return *_memory; }
		const Memory& memory() const { return *_memory; }

	public:
		friend class IOPorts;
	};

	class DummyDevice final : public IODevice
	{
		u8 readPortUnsignedByte(PortNumber port) override { return 0xFF; }
		i8 readPortSignedByte(PortNumber port) override { return -1; }
		u16 readPortUnsignedHalfword(PortNumber port) override { return 0xFFFF; }
		i16 readPortSignedHalfword(PortNumber port) override { return -1; }
		u32 readPortUnsignedWord(PortNumber port) override { return 0xFFFFFFFF; }
		void readPort(PortNumber port, Address address, u32 size) override
		{
			memory().setBytes(address, 0xFF, size); // Fill the memory with 0xFF for undefined ports.
		}

		void writePortByte(PortNumber port, u8 value) override {}
		void writePortHalfword(PortNumber port, u16 value) override {}
		void writePortWord(PortNumber port, u32 value) override {}
		void writePort(PortNumber port, Address address, u32 size) override {}
	};

	class IOPorts
	{
	public:
		static inline constexpr usize MaxPorts = 256;

	private:
		std::array<IODevice*, MaxPorts> _devices{};
		Memory& _memory;

	public:
		IOPorts() = delete;
		IOPorts(const IOPorts&) = delete;
		IOPorts(IOPorts&&) = delete;
		~IOPorts() = default;

		IOPorts& operator=(const IOPorts&) = delete;
		IOPorts& operator=(IOPorts&&) = delete;

	public:
		IOPorts(Memory& memory) : _memory(memory)
		{
			_devices.fill(nullptr);
		}

		inline constexpr void attach(PortNumber port, IODevice& device)
		{
			_devices[port] = &device;
			device._memory = &_memory; // Set the memory reference for the device
		}

		inline constexpr void attachRange(PortNumber start, PortNumber end, IODevice& device)
		{
			for (PortNumber port = start; port <= end; ++port)
			{
				_devices[port] = &device;
				device._memory = &_memory; // Set the memory reference for the device
			}
		}

		inline constexpr void detach(PortNumber port)
		{
			if (IODevice* device = _devices[port])
			{
				device->_memory = nullptr; // Clear the memory reference for the device
				_devices[port] = nullptr;
			}
		}

		inline constexpr void detachRange(PortNumber start, PortNumber end)
		{
			for (PortNumber port = start; port <= end; ++port)
			{
				if (IODevice* device = _devices[port])
				{
					device->_memory = nullptr; // Clear the memory reference for the device
					_devices[port] = nullptr;
				}
			}
		}

		inline constexpr bool isAttached(PortNumber port) const
		{
			return _devices[port] != nullptr;
		}

		template <Integral T> requires (sizeof(T) <= sizeof(u32))
		forceinline T read(PortNumber port)
		{
			if (IODevice* device = _devices[port])
			{
				if constexpr (SignedIntegral<T>)
				{
					if constexpr (sizeof(T) == 1)
						return device->readPortSignedByte(port);
					else if constexpr (sizeof(T) == 2)
						return device->readPortSignedHalfword(port);
					else
						static_assert(false, "Unsupported signed integral type size for port read");
				}
				else
				{
					if constexpr (sizeof(T) == 1)
						return device->readPortUnsignedByte(port);
					else if constexpr (sizeof(T) == 2)
						return device->readPortUnsignedHalfword(port);
					else if constexpr (sizeof(T) == 4)
						return device->readPortUnsignedWord(port);
					else
						static_assert(false, "Unsupported unsigned integral type size for port read");
				}
			}
			if constexpr (SignedIntegral<T>)
				return static_cast<T>(-1); // Default to returning -1 for signed types if no device is attached
			else
				return std::numeric_limits<T>::max(); // Default to returning the maximum value if no device is attached
		}

		template <UnsignedIntegral T> requires (sizeof(T) <= sizeof(u32))
		forceinline void write(PortNumber port, T value)
		{
			if (IODevice* device = _devices[port])
			{
				if constexpr (sizeof(T) == 1)
					device->writePortByte(port, value);
				else if constexpr (sizeof(T) == 2)
					device->writePortHalfword(port, value);
				else if constexpr (sizeof(T) == 4)
					device->writePortWord(port, value);
				else
					static_assert(false, "Unsupported integral type size for port write");
			}
		}

		forceinline void readBytes(PortNumber port, Address address, u32 size)
		{
			if (IODevice* device = _devices[port])
			{
				device->readPort(port, address, size);
			}
			else
			{
				_memory.setBytes(address, 0xFF, size); // Fill the memory with 0xFF for undefined ports.
			}
		}

		forceinline void writeBytes(PortNumber port, Address address, u32 size)
		{
			if (IODevice* device = _devices[port])
			{
				device->writePort(port, address, size);
			}
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

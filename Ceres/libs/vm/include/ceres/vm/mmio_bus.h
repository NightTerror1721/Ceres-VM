#pragma once

#include <ceres/core/isa/address.h>
#include "memory.h"
#include "interrupt_controller.h"
#include <algorithm>
#include <array>
#include <limits>
#include <mutex>

namespace ceres::vm
{
	class MmioBus;

	// A device's registers, reached through ordinary loads and stores instead of a separate `in`/
	// `out` address space. Where the old port interface took a PortNumber, this one takes an
	// `Address` already relative to the device's own slot (see MmioBus) — a device never sees the
	// 0xFF000000 base, only its own offsets from 0x00000000 up.
	class IODevice
	{
	private:
		Memory* _memory = nullptr;
		InterruptController* _interrupts = nullptr;
		// A host input thread may call raiseInterrupt while a machine is being torn down.
		// The bus clears both pointers under this lock before the VM can be destroyed.
		std::mutex _connectionMutex;

	public:
		virtual ~IODevice() = default;

	public:
		virtual u8 readUnsignedByte(Address offset) = 0;
		virtual i8 readSignedByte(Address offset) = 0;
		virtual u16 readUnsignedHalfword(Address offset) = 0;
		virtual i16 readSignedHalfword(Address offset) = 0;
		virtual u32 readUnsignedWord(Address offset) = 0;

		virtual void writeByte(Address offset, u8 value) = 0;
		virtual void writeHalfword(Address offset, u16 value) = 0;
		virtual void writeWord(Address offset, u32 value) = 0;

	public:
		// Opt-in: only a device whose needsTick() returns true is called every instruction (see
		// MmioBus::rebuildTickedDevices). Most devices have no notion of time - the terminal, the
		// disk, the framebuffer - and paid for a virtual call every instruction regardless; this is
		// what lets them stop paying for it without their tick() ever having to be touched.
		virtual bool needsTick() const noexcept { return false; }

		// Called once per executed instruction, but only for a device that opted in via needsTick().
		// The default does nothing, which is also what a device that never overrides either method
		// gets: no per-instruction cost at all, not even the call.
		virtual void tick() {}

	protected:
		Memory& memory() { return *_memory; }
		const Memory& memory() const { return *_memory; }

		// How a device speaks first.
		void raiseInterrupt(InterruptNumber interruptNumber)
		{
			const std::lock_guard lock{_connectionMutex};
			if (InterruptController* interrupts = _interrupts)
				interrupts->raise(interruptNumber);
		}

	public:
		friend class MmioBus;
	};

	class DummyDevice final : public IODevice
	{
		u8 readUnsignedByte(Address) override { return 0xFF; }
		i8 readSignedByte(Address) override { return -1; }
		u16 readUnsignedHalfword(Address) override { return 0xFFFF; }
		i16 readSignedHalfword(Address) override { return -1; }
		u32 readUnsignedWord(Address) override { return 0xFFFFFFFF; }

		void writeByte(Address, u8) override {}
		void writeHalfword(Address, u16) override {}
		void writeWord(Address, u32) override {}
	};

	// The top 16 MiB of the 32-bit address space, reserved exclusively for devices. It costs no real
	// RAM in any configuration: Memory::MaxSize is 1 GiB (0x40000000), nowhere near 0xFF000000, so
	// this window can never collide with a program's own memory regardless of how much RAM the
	// machine was given. Split into 256 slots of 64 KiB each — the same device-count limit
	// IOPorts::MaxPorts used to impose, but each device now gets a whole window of ordinary,
	// word-addressable registers instead of a handful of single-byte port numbers.
	class MmioBus
	{
	public:
		static inline constexpr u32 BaseValue = 0xFF000000;
		static inline constexpr Address Base = Address(BaseValue);
		static inline constexpr u32 SlotSize = 0x00010000; // 64 KiB
		static inline constexpr usize MaxDevices = 256;

		static forceinline constexpr Address slot(u32 index) noexcept
		{
			return Address(BaseValue + index * SlotSize);
		}

	private:
		std::array<IODevice*, MaxDevices> _devices{};
		// Rebuilt only when the topology changes. The execution engine ticks this list after
		// every instruction, so it must not scan the entire MMIO address space each time.
		std::array<IODevice*, MaxDevices> _tickedDevices{};
		usize _tickedDeviceCount = 0;
		Memory& _memory;
		InterruptController& _interrupts;

		void rebuildTickedDevices()
		{
			_tickedDeviceCount = 0;
			for (IODevice* device : _devices)
			{
				if (device == nullptr || !device->needsTick())
					continue;

				bool alreadyListed = false;
				for (usize i = 0; i < _tickedDeviceCount; ++i)
				{
					if (_tickedDevices[i] == device)
					{
						alreadyListed = true;
						break;
					}
				}

				if (!alreadyListed)
					_tickedDevices[_tickedDeviceCount++] = device;
			}
		}

	public:
		MmioBus() = delete;
		MmioBus(const MmioBus&) = delete;
		MmioBus(MmioBus&&) = delete;
		~MmioBus() = default;

		MmioBus& operator=(const MmioBus&) = delete;
		MmioBus& operator=(MmioBus&&) = delete;

	public:
		MmioBus(Memory& memory, InterruptController& interrupts) : _memory(memory), _interrupts(interrupts)
		{
			_devices.fill(nullptr);
		}

		// Whether a (physical) address falls inside the reserved window at all - the one check
		// ExecutionEngine needs before routing a load/store here instead of to Memory.
		static forceinline constexpr bool contains(Address address) noexcept
		{
			return address.value() >= BaseValue;
		}

		// One pulse per executed instruction, delivered once per device however many slots it
		// claims (attachRange gives one device several contiguous slots for a single logical
		// component - the DMA controller does not need this, but a future device might).
		void tick()
		{
			for (usize i = 0; i < _tickedDeviceCount; ++i)
				_tickedDevices[i]->tick();
		}

		inline constexpr void attach(Address base, IODevice& device)
		{
			const u32 index = (base.value() - BaseValue) / SlotSize;
			_devices[index] = &device;
			const std::lock_guard lock{device._connectionMutex};
			device._memory = &_memory;
			device._interrupts = &_interrupts;
			rebuildTickedDevices();
		}

		inline constexpr void attachRange(Address firstBase, Address lastBase, IODevice& device)
		{
			const u32 first = (firstBase.value() - BaseValue) / SlotSize;
			const u32 last = (lastBase.value() - BaseValue) / SlotSize;
			for (u32 index = first; index <= last; ++index)
			{
				_devices[index] = &device;
				const std::lock_guard lock{device._connectionMutex};
				device._memory = &_memory;
				device._interrupts = &_interrupts;
			}
			rebuildTickedDevices();
		}

		inline constexpr void detach(Address base)
		{
			const u32 index = (base.value() - BaseValue) / SlotSize;
			if (IODevice* device = _devices[index])
			{
				_devices[index] = nullptr;
				if (std::find(_devices.begin(), _devices.end(), device) == _devices.end())
				{
					const std::lock_guard lock{device->_connectionMutex};
					device->_memory = nullptr;
					device->_interrupts = nullptr;
				}
				rebuildTickedDevices();
			}
		}

		inline constexpr bool isAttached(Address base) const
		{
			const u32 index = (base.value() - BaseValue) / SlotSize;
			return _devices[index] != nullptr;
		}

	public:
		template <Integral T> requires (sizeof(T) <= sizeof(u32))
		forceinline T read(Address address)
		{
			const u32 relative = address.value() - BaseValue;
			const u32 index = relative / SlotSize;
			const Address offset = Address(relative % SlotSize);

			if (IODevice* device = _devices[index])
			{
				if constexpr (SignedIntegral<T>)
				{
					if constexpr (sizeof(T) == 1)
						return device->readSignedByte(offset);
					else if constexpr (sizeof(T) == 2)
						return device->readSignedHalfword(offset);
					else
						static_assert(false, "Unsupported signed integral type size for MMIO read");
				}
				else
				{
					if constexpr (sizeof(T) == 1)
						return device->readUnsignedByte(offset);
					else if constexpr (sizeof(T) == 2)
						return device->readUnsignedHalfword(offset);
					else if constexpr (sizeof(T) == 4)
						return device->readUnsignedWord(offset);
					else
						static_assert(false, "Unsupported unsigned integral type size for MMIO read");
				}
			}

			if constexpr (SignedIntegral<T>)
				return static_cast<T>(-1); // Same convention an unattached port used: all-ones.
			else
				return std::numeric_limits<T>::max();
		}

		template <UnsignedIntegral T> requires (sizeof(T) <= sizeof(u32))
		forceinline void write(Address address, T value)
		{
			const u32 relative = address.value() - BaseValue;
			const u32 index = relative / SlotSize;
			const Address offset = Address(relative % SlotSize);

			if (IODevice* device = _devices[index])
			{
				if constexpr (sizeof(T) == 1)
					device->writeByte(offset, value);
				else if constexpr (sizeof(T) == 2)
					device->writeHalfword(offset, value);
				else if constexpr (sizeof(T) == 4)
					device->writeWord(offset, value);
				else
					static_assert(false, "Unsupported integral type size for MMIO write");
			}
			// Writing an unattached slot is silently discarded, as it always was.
		}
	};

	// Base addresses of the devices Ceres ships with, one 64 KiB slot each. Ports 0x70-0xFE's worth
	// of headroom in the old scheme is now slots 5-254 - reserved for whatever comes next.
	namespace default_mmio
	{
		using vm::MmioBus;

		static inline constexpr Address Terminal = MmioBus::slot(0);
		static inline constexpr Address Timer = MmioBus::slot(1);
		static inline constexpr Address Disk = MmioBus::slot(2);
		static inline constexpr Address Framebuffer = MmioBus::slot(3);
		static inline constexpr Address Dma = MmioBus::slot(4);
		// Slots 5-254 reserved for future default devices.
		static inline constexpr Address SystemControl = MmioBus::slot(255);
	}
}

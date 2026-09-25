#pragma once

#include <ceres/core/isa/address.h>
#include "memory.h"
#include "interrupt_controller.h"
#include "io_device.h"
#include <algorithm>
#include <array>
#include <limits>
#include <mutex>

namespace ceres::vm
{
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

		// The nearest thing any ticked device has scheduled, in ticks; NoDeviceEvent when none has.
		u64 ticksUntilNextEvent() const noexcept
		{
			u64 nearest = NoDeviceEvent;
			for (usize i = 0; i < _tickedDeviceCount; ++i)
			{
				const u64 next = _tickedDevices[i]->ticksUntilEvent();
				if (next < nearest)
					nearest = next;
			}
			return nearest;
		}

		// `ticks` pulses at once, for a halted machine whose clock ran on without it: never more than
		// ticksUntilNextEvent(), so no device skips past something it had to do.
		void advance(u64 ticks)
		{
			const u64 nearest = ticksUntilNextEvent();
			if (ticks > nearest)
				ticks = nearest;                   // no device steps past something it had to do
			if (ticks == 0)
				return;
			for (usize i = 0; i < _tickedDeviceCount; ++i)
				_tickedDevices[i]->advance(ticks);
		}

		// Every attached device's reset(), once per device however many slots it claims.
		void resetDevices()
		{
			for (usize slotIndex = 0; slotIndex < MaxDevices; ++slotIndex)
			{
				IODevice* device = _devices[slotIndex];
				if (device == nullptr)
					continue;
				bool seen = false;
				for (usize earlier = 0; earlier < slotIndex; ++earlier)
				{
					if (_devices[earlier] == device)
					{
						seen = true;
						break;
					}
				}
				if (!seen)
					device->reset();
			}
		}

		void attach(Address base, IODevice& device)
		{
			const u32 index = (base.value() - BaseValue) / SlotSize;
			_devices[index] = &device;
			const std::lock_guard lock{device._connectionMutex};
			device._memory = &_memory;
			device._interrupts = &_interrupts;
			rebuildTickedDevices();
		}

		void attachRange(Address firstBase, Address lastBase, IODevice& device)
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

		void detach(Address base)
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
		// One aligned 32-bit access: the execution engine has already faulted every other kind (plan/v2 SPEC 5.1).
		forceinline u32 read(Address address)
		{
			const u32 relative = address.value() - BaseValue;
			if (IODevice* device = _devices[relative / SlotSize])
				return device->read(Address(relative % SlotSize));
			return 0xFFFFFFFF; // an empty slot reads all ones, as an unattached port always did
		}

		forceinline void write(Address address, u32 value)
		{
			const u32 relative = address.value() - BaseValue;
			if (IODevice* device = _devices[relative / SlotSize])
				device->write(Address(relative % SlotSize), value);
			// Writing an empty slot is silently discarded, as it always was.
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
		static inline constexpr Address Keyboard = MmioBus::slot(5);
		static inline constexpr Address Mouse = MmioBus::slot(6);
		static inline constexpr Address Display = MmioBus::slot(7);
		static inline constexpr Address Gamepad = MmioBus::slot(8);
		static inline constexpr Address Audio = MmioBus::slot(9);
		static inline constexpr Address Peripherals = MmioBus::slot(10);
		static inline constexpr Address HostFs = MmioBus::slot(11);
		static inline constexpr Address Blitter = MmioBus::slot(12);
		// Slots 13-254 reserved for future default devices.
		static inline constexpr Address SystemControl = MmioBus::slot(255);
	}
}

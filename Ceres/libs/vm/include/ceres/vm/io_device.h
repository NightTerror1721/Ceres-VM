#pragma once

#include <ceres/core/base/types.h>
#include <ceres/core/isa/address.h>
#include "memory.h"
#include "interrupt_controller.h"
#include "register_map.h"
#include <mutex>

// A device as the bus sees it: the interface every device implements, and how it reaches the machine.
namespace ceres::vm
{
	class MmioBus;

	// How fast a halted machine's clock runs: the ticks per second a device that keeps time (the
	// timer) sees while the CPU sleeps in HALT, so a wait of N ticks is N instructions' worth of time
	// whether the machine is running or halted. About what the interpreter executes per second.
	inline constexpr u64 DefaultHaltClockHz = 100'000'000;

	// "Nothing scheduled" for IODevice::ticksUntilEvent.
	inline constexpr u64 NoDeviceEvent = ~u64{ 0 };

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

		// How many ticks from now this device will next act on its own - raise its interrupt, land a
		// transfer - or NoDeviceEvent when nothing is scheduled. A halted machine uses it to sleep until
		// then instead of ticking one step at a time. Only a device that opted into tick() is asked.
		virtual u64 ticksUntilEvent() const noexcept { return NoDeviceEvent; }

		// `ticks` ticks at once, never more than ticksUntilEvent() said: exactly what that many tick()
		// calls would do. The default makes the calls, which is right but slow for a device that is
		// asked to skip far; every device that keeps time overrides it.
		virtual void advance(u64 ticks)
		{
			for (u64 i = 0; i < ticks; ++i)
				tick();
		}

		// The machine is starting over (the system control device's reset command): put back whatever
		// would otherwise reach into the fresh program - an armed timer, a transfer in flight, a tone
		// still playing, a feature switched on. What the host plugged in or typed stays: a reset
		// restarts the machine, it does not unplug it. The default keeps everything.
		virtual void reset() {}

		// The device's registers, declared in one table (register_map.h). Empty until every device declares its
		// own (plan/v2 F1.8), when this becomes pure.
		virtual const RegisterMap& registers() const
		{
			static constexpr RegisterMap none{};
			return none;
		}

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
}

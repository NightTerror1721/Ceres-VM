#pragma once

#include <ceres/core/base/types.h>
#include <ceres/core/isa/address.h>
#include "memory.h"
#include "interrupt_controller.h"
#include "register_map.h"
#include "scheduler.h"
#include <mutex>

// A device as the bus sees it: the interface every device implements, and how it reaches the machine.
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
		Scheduler* _scheduler = nullptr;   // set by the bus on attach, like the two above
		// A host input thread may call raiseInterrupt while a machine is being torn down.
		// The bus clears both pointers under this lock before the VM can be destroyed.
		std::mutex _connectionMutex;

	public:
		virtual ~IODevice() = default;

	public:
		// Every register is 32 bits wide and is reached with an aligned 32-bit access; the bus faults any other
		// access before it gets here (plan/v2 SPEC 5.1). `offset` is relative to the device's slot.
		virtual u32 read(Address offset) = 0;
		virtual void write(Address offset, u32 value) = 0;

	public:
		// An event this device scheduled (scheduler().schedule) has come due: `cycle` is the one it asked for,
		// and the clock may be a few cycles past it - it is looked at between instructions. This is how a device
		// acts on its own - a timer running out, a transfer landing; there is no per-instruction call.
		virtual void onEvent([[maybe_unused]] u32 tag, [[maybe_unused]] u64 cycle) {}

		// The machine is starting over (the system control device's reset command): put back whatever
		// would otherwise reach into the fresh program - an armed timer, a transfer in flight, a tone
		// still playing, a feature switched on. What the host plugged in or typed stays: a reset
		// restarts the machine, it does not unplug it. The default keeps everything.
		virtual void reset() {}

		// The device's registers, declared in one table (register_map.h, plan/v2 SPEC 5.3). The bus reads it when
		// the device is attached: an offset missing from it reads 0 and ignores writes, or faults under --strict-mmio.
		virtual const RegisterMap& registers() const = 0;

	protected:
		Memory& memory() { return *_memory; }
		const Memory& memory() const { return *_memory; }

		// The machine's event scheduler, or nullptr while the device is not attached to a bus.
		Scheduler* scheduler() noexcept { return _scheduler; }
		const Scheduler* scheduler() const noexcept { return _scheduler; }

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

}

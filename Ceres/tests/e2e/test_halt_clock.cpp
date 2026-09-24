// A halted machine keeps instruction time: its clock runs on at the halt clock's rate (ticks per second)
// instead of one tick per millisecond, the host sleeps until the next device event instead of stepping
// through the ticks, and anything a device raises wakes it at once.

#include "framework.h"
#include <ceres/vm/ceresvm.h>
#include <ceres/devices/devices.h>
#include <ceres/vm/bios.h>

#include <chrono>
#include <thread>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::devices;
using namespace ceres::testing;

namespace
{
	using Clock = std::chrono::steady_clock;

	// STI, HALT, then a marker; the timer's handler (at 0x800) sets r9 and returns.
	class HaltedMachine
	{
	private:
		CeresVM _vm;

	public:
		TimerDevice timer;

		HaltedMachine()
		{
			const Address entry = Memory::UnrestrictedSegmentStart;
			const Instruction program[] = { Instruction::STI(), Instruction::HALT(), Instruction::LI(10, 0x33) };
			for (usize i = 0; i < std::size(program); ++i)
				_vm.memory().writeUnchecked<u32>(entry + Address(static_cast<u32>(i * Instruction::Size)), program[i].raw());

			const Address handler{ 0x800 };
			_vm.memory().writeUnchecked<u32>(handler, Instruction::LI(9, 0x5A).raw());
			_vm.memory().writeUnchecked<u32>(handler + Address(4), Instruction::IRET().raw());

			BIOS bios{};
			bios.initializeMemory(_vm.memory());
			_vm.memory().writeUnchecked<u32>(0_addr, entry.value());
			_vm.memory().writeUnchecked<u32>(Address(static_cast<u32>(TimerDevice::Interrupt) * Address::Size), handler.value());
			_vm.engine().reset();
			timer.attachTo(_vm.io());
		}

		~HaltedMachine() { timer.detachFrom(_vm.io()); }

		CeresVM& vm() noexcept { return _vm; }
		void step() { _vm.engine().step(); }
		u32 reg(usize index) const { return _vm.engine().registers().getValue(index); }
		bool halted() const noexcept { return _vm.engine().isHalted(); }

		// Runs STI and HALT: the machine is halted afterwards.
		void runToHalt()
		{
			step();
			step();
		}
	};
}

TEST(halt_clock, with_the_clock_off_one_halted_step_reaches_the_timer_exactly)
{
	HaltedMachine m;
	m.vm().engine().setHaltClock(0);
	m.timer.arm(1'000'000);
	m.runToHalt();
	CHECK(m.halted());

	const u64 before = m.timer.ticks();
	const u64 remaining = m.timer.captureState().remaining;
	m.step();                                   // one step: the whole wait
	CHECK_EQ(m.timer.ticks(), before + remaining);
	CHECK(!m.timer.isArmed());
	m.step();                                   // the interrupt is delivered and wakes it
	m.step();
	m.step();
	CHECK_EQ(m.reg(9), 0x5Au);
	CHECK_EQ(m.reg(10), 0x33u);
}

TEST(halt_clock, a_halted_wait_takes_its_ticks_in_real_time_at_the_halt_clock)
{
	HaltedMachine m;
	m.vm().engine().setHaltClock(1'000'000);    // a microsecond a tick
	m.timer.arm(30'000);                        // 30 ms
	const Clock::time_point start = Clock::now();
	m.runToHalt();
	const u64 before = m.timer.ticks();

	int steps = 0;
	while (m.halted() && steps < 300)             // a regression fails in seconds, not minutes
	{
		m.step();
		++steps;
	}
	const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();

	CHECK(!m.halted());
	CHECK(waited >= 25);                        // it did wait, for about the ticks' worth of time
	CHECK(waited < 2000);
	CHECK(steps < 100);                         // in a few sleeps, not a step per tick
	CHECK(m.timer.ticks() - before <= 30'000);  // the clock stopped at the event, not past it
	CHECK_EQ(m.reg(9), 0x5Au);
}

TEST(halt_clock, a_raise_from_another_thread_wakes_a_halted_machine_at_once)
{
	HaltedMachine m;                            // nothing armed: only the host can wake it
	m.runToHalt();
	CHECK(m.halted());

	// The terminal's interrupt, raised the way a device's input thread raises it.
	m.vm().memory().writeUnchecked<u32>(Address(static_cast<u32>(TerminalDevice::Interrupt) * Address::Size), 0x800u);
	std::thread raiser([&vm = m.vm()]
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		vm.interrupts().raise(TerminalDevice::Interrupt);
	});

	const Clock::time_point start = Clock::now();
	int steps = 0;
	while (m.halted() && steps < 1000)
	{
		m.step();
		++steps;
	}
	raiser.join();
	const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();

	CHECK(!m.halted());
	CHECK(waited < 1000);
	CHECK(steps < 20);                          // ~3 sleeps of 10 ms and the delivery, not 30 steps of 1 ms
}

TEST(halt_clock, a_masked_request_wakes_a_halt_once_without_being_taken)
{
	HaltedMachine m;
	m.runToHalt();
	m.vm().engine().setFlags(FlagRegister{});   // interrupts masked, and not halted any more ...
	m.vm().memory().writeUnchecked<u32>(Address(0x404), Instruction::HALT().raw());
	m.vm().memory().writeUnchecked<u32>(Address(0x408), Instruction::HALT().raw());
	m.vm().engine().setProgramCounter(Address(0x404));
	m.step();                                   // ... until this HALT
	CHECK(m.halted());
	m.vm().interrupts().raise(TimerDevice::Interrupt);   // masked: it cannot be taken

	m.step();                                   // it wakes the halt, like ARM's WFI, and runs the next HALT
	CHECK_EQ(m.vm().engine().programCounter().value(), 0x40Cu);
	CHECK(m.vm().interrupts().hasPending());    // still there for when the program unmasks it
	CHECK(m.reg(9) != 0x5Au);                   // and its handler did not run

	// Still pending, but it has woken one halt already: the second sleeps its full slice.
	CHECK(m.halted());
	const Clock::time_point start = Clock::now();
	m.step();
	const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
	CHECK(m.halted());
	CHECK(waited >= 5);
}

TEST(halt_clock, a_request_raised_before_the_halt_keeps_it_from_sleeping)
{
	// The race a masked sleep has to survive: the event comes after the program last looked but
	// before its HALT has run. The HALT finds it and goes straight on.
	HaltedMachine m;
	m.vm().memory().writeUnchecked<u32>(Address(0x404), Instruction::HALT().raw());
	m.vm().engine().setFlags(FlagRegister{});
	m.vm().engine().setProgramCounter(Address(0x404));
	m.vm().interrupts().raise(TimerDevice::Interrupt);
	m.step();
	CHECK(!m.halted());
	CHECK_EQ(m.vm().engine().programCounter().value(), 0x408u);
}

TEST(halt_clock, a_request_with_no_handler_still_wakes_the_halt)
{
	// Interrupts enabled, but nothing bound to the vector: the request is dropped as before, and the
	// halt still ends rather than sleeping on for good.
	HaltedMachine m;
	m.runToHalt();
	CHECK(m.halted());
	m.vm().interrupts().raise(TimerDevice::AlarmInterrupt);   // vector 24 is 0
	m.step();
	CHECK(!m.halted());
	CHECK_EQ(m.reg(10), 0x33u);                 // the instruction after the HALT ran
}

TEST(halt_clock, a_halt_after_an_interrupt_was_taken_waits_for_the_next_one)
{
	// Taking an interrupt is the wake-up: the HALT after the handler's IRET sleeps again.
	HaltedMachine m;
	m.vm().memory().writeUnchecked<u32>(Address(0x40C), Instruction::HALT().raw());   // after the LI of r10
	m.runToHalt();
	m.vm().interrupts().raise(TimerDevice::Interrupt);
	for (int i = 0; i < 3; ++i)                 // delivered with the handler's LI, its IRET, the LI of r10
		m.step();
	CHECK_EQ(m.reg(9), 0x5Au);
	CHECK_EQ(m.reg(10), 0x33u);
	m.step();                                   // the second HALT
	CHECK(m.halted());
	CHECK(!m.vm().engine().hasWakeEvent());
}

TEST(halt_clock, the_alarm_fires_at_its_instant_on_the_nanosecond_clock)
{
	TimerDevice timer;
	CHECK_EQ(timer.alarmNanos(), u64{ 0 });
	const u64 soon = static_cast<u64>(timer.readUnsignedWord(TimerDevice::NanosLowRegister)) |
		(static_cast<u64>(timer.readUnsignedWord(TimerDevice::NanosHighRegister)) << 32);
	const u64 at = soon + 20'000'000;           // 20 ms from now
	timer.writeWord(TimerDevice::AlarmLowRegister, static_cast<u32>(at));
	CHECK_EQ(timer.alarmNanos(), u64{ 0 });     // the low word alone does not arm it
	CHECK_EQ(timer.readUnsignedWord(TimerDevice::AlarmLowRegister), 0u);   // and a disarmed alarm reads 0:0
	timer.writeWord(TimerDevice::AlarmHighRegister, static_cast<u32>(at >> 32));
	CHECK_EQ(timer.alarmNanos(), at);
	CHECK_EQ(timer.readUnsignedWord(TimerDevice::AlarmLowRegister), static_cast<u32>(at));

	// About 20 ms of halted-clock ticks, rounded up, and never 0.
	const u64 ticks = timer.ticksUntilEvent();
	CHECK(ticks > 0 && ticks <= 20'000'000ull * DefaultHaltClockHz / 1'000'000'000ull + 1);
	timer.setHaltClockRate(0);
	CHECK_EQ(timer.ticksUntilEvent(), NoDeviceEvent);   // no real time while halted: no event to offer

	timer.writeWord(TimerDevice::AlarmLowRegister, 0);
	timer.writeWord(TimerDevice::AlarmHighRegister, 0);
	CHECK_EQ(timer.alarmNanos(), u64{ 0 });     // 0:0 disarms
}

TEST(halt_clock, a_masked_halt_sleeps_until_the_alarm_in_real_time)
{
	// No handler, interrupts masked: arm the alarm, halt, and the alarm's own request wakes it.
	HaltedMachine m;
	m.vm().memory().writeUnchecked<u32>(Address(0x404), Instruction::HALT().raw());
	m.vm().engine().setFlags(FlagRegister{});
	m.vm().engine().setProgramCounter(Address(0x404));
	const u64 now = static_cast<u64>(m.timer.readUnsignedWord(TimerDevice::NanosLowRegister)) |
		(static_cast<u64>(m.timer.readUnsignedWord(TimerDevice::NanosHighRegister)) << 32);
	const u64 at = now + 30'000'000;            // 30 ms
	m.timer.writeWord(TimerDevice::AlarmLowRegister, static_cast<u32>(at));
	m.timer.writeWord(TimerDevice::AlarmHighRegister, static_cast<u32>(at >> 32));

	const Clock::time_point start = Clock::now();
	m.step();
	CHECK(m.halted());
	int steps = 0;
	while (m.halted() && steps < 300)
	{
		m.step();
		++steps;
	}
	const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
	CHECK(!m.halted());
	CHECK(waited >= 25);
	CHECK(waited < 2000);
	CHECK(steps < 20);                          // a few sleeps of up to 10 ms, not a poll per tick
	CHECK_EQ(m.timer.alarmNanos(), u64{ 0 });   // it fired once and disarmed
	CHECK(m.vm().interrupts().peek() == TimerDevice::AlarmInterrupt);
}

TEST(halt_clock, a_slow_halt_clock_still_reaches_the_event)
{
	// At 50 Hz a 10 ms sleep is half a tick: the remainder has to carry, or the clock never moves.
	HaltedMachine m;
	m.vm().engine().setHaltClock(50);
	m.timer.arm(3);                             // STI and HALT take two; one tick of 20 ms is left
	m.runToHalt();
	int steps = 0;
	while (m.halted() && steps < 300)
	{
		m.step();
		++steps;
	}
	CHECK(!m.halted());
	CHECK_EQ(m.reg(9), 0x5Au);
}

TEST(halt_clock, the_timer_reports_the_halt_clock_it_was_told)
{
	TimerDevice timer;
	CHECK_EQ(timer.readUnsignedWord(TimerDevice::HaltClockRegister), static_cast<u32>(DefaultHaltClockHz));
	timer.setHaltClockRate(0);
	CHECK_EQ(timer.readUnsignedWord(TimerDevice::HaltClockRegister), 0u);
}

TEST(halt_clock, advancing_the_timer_is_the_same_as_ticking_it)
{
	TimerDevice ticked;
	TimerDevice skipped;
	ticked.arm(100, true);
	skipped.arm(100, true);
	for (int i = 0; i < 100; ++i)
		ticked.tick();
	skipped.advance(100);
	CHECK_EQ(ticked.ticks(), skipped.ticks());
	CHECK_EQ(ticked.captureState().remaining, skipped.captureState().remaining);   // periodic: re-armed
	CHECK_EQ(skipped.ticksUntilEvent(), u64{ 100 });
	skipped.advance(40);
	CHECK_EQ(skipped.ticksUntilEvent(), u64{ 60 });
	TimerDevice idle;
	CHECK_EQ(idle.ticksUntilEvent(), NoDeviceEvent);
}

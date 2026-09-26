// A halted machine and the event scheduler: the clock jumps straight to the next device event, with nothing
// scheduled the host's raise is all that can wake it, and a halt ends on any request raised, taken or not.

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
		u64 cycles() const noexcept { return _vm.engine().cycles(); }

		// Runs STI and HALT: the machine is halted afterwards.
		void runToHalt()
		{
			step();
			step();
		}

		void armAlarm(u64 at)
		{
			timer.write(TimerDevice::AlarmLowRegister, static_cast<u32>(at));
			timer.write(TimerDevice::AlarmHighRegister, static_cast<u32>(at >> 32));
		}
	};
}

TEST(halted_machine, one_halted_step_reaches_the_timer_exactly)
{
	HaltedMachine m;
	m.timer.arm(1'000'000);
	m.runToHalt();
	CHECK(m.halted());

	const u64 remaining = m.timer.captureState().remaining;
	const u64 before = m.cycles();
	m.step();                                   // one step: the whole wait
	CHECK_EQ(m.cycles(), before + remaining);
	CHECK(!m.timer.isArmed());
	m.step();                                   // the interrupt is delivered and wakes it
	m.step();
	m.step();
	CHECK_EQ(m.reg(9), 0x5Au);
	CHECK_EQ(m.reg(10), 0x33u);
}

TEST(halted_machine, a_long_wait_takes_no_host_time)
{
	// A minute of the machine's time is one jump of its clock: pacing it against the host is the runner's job.
	HaltedMachine m;
	m.timer.arm(60 * DefaultCpuClockHz / 2);    // the countdown takes 31 bits: half a minute
	const Clock::time_point start = Clock::now();
	m.runToHalt();
	int steps = 0;
	while (m.halted() && steps < 10)
	{
		m.step();
		++steps;
	}
	const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
	CHECK(!m.halted());
	CHECK(steps <= 2);
	CHECK(waited < 1000);
	CHECK(m.timer.nanos() >= 30'000'000'000ull);
}

TEST(halted_machine, a_raise_from_another_thread_wakes_a_halted_machine_at_once)
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
	const u64 before = m.cycles();
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
	CHECK(steps < 20);                          // ~3 waits of 10 ms and the delivery, not a busy loop
	CHECK(m.cycles() - before <= isa::cycles::InterruptEntry + 1);   // no event: the clock did not move while it waited
}

TEST(halted_machine, a_masked_request_wakes_a_halt_once_without_being_taken)
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

	// Still pending, but it has woken one halt already: the second waits its full slice for the host.
	CHECK(m.halted());
	const Clock::time_point start = Clock::now();
	m.step();
	const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
	CHECK(m.halted());
	CHECK(waited >= 5);
}

TEST(halted_machine, a_request_raised_before_the_halt_keeps_it_from_sleeping)
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

TEST(halted_machine, a_request_with_no_handler_still_wakes_the_halt)
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

TEST(halted_machine, a_halt_after_an_interrupt_was_taken_waits_for_the_next_one)
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

TEST(halted_machine, a_running_machine_reaches_the_alarm_and_takes_its_interrupt)
{
	// STI, then a jump to itself: the program runs, never halts, and the alarm's handler (bound to 24) runs
	// on the cycle its instant falls on.
	HaltedMachine m;
	m.vm().memory().writeUnchecked<u32>(Memory::UnrestrictedSegmentStart + Address(4), Instruction::JP(i24(0)).raw());
	m.vm().memory().writeUnchecked<u32>(Address(static_cast<u32>(TimerDevice::AlarmInterrupt) * Address::Size), 0x800u);
	const u64 at = m.timer.nanos() + 2'000'000;  // 2 ms: 100 000 cycles, 50 000 jumps of 2
	m.armAlarm(at);
	u64 steps = 0;
	while (m.reg(9) != 0x5Au && steps < 1'000'000)
	{
		m.step();
		++steps;
	}
	CHECK_EQ(m.reg(9), 0x5Au);
	CHECK(!m.halted());
	CHECK(m.timer.nanos() >= at);               // not before its instant
	CHECK(steps < 50'010);                      // and on the cycle it falls on, give or take the handler
	CHECK_EQ(m.timer.alarmNanos(), u64{ 0 });
}

TEST(halted_machine, a_trap_left_set_does_not_keep_a_masked_halt_asleep)
{
	// A division by zero with the fault switched off sets Trap and goes on; the flag stays. A masked halt
	// afterwards must still end on a request - only a machine stopped for want of stack stays stopped.
	HaltedMachine m;
	m.vm().memory().writeUnchecked<u32>(Address(0x404), Instruction::HALT().raw());
	m.vm().engine().setFlags(FlagRegister{ static_cast<FlagRegister::ValueType>(ExecutionFlag::Trap) });
	m.vm().engine().setProgramCounter(Address(0x404));
	m.step();
	CHECK(m.halted());
	m.vm().interrupts().raise(TimerDevice::Interrupt);
	m.step();
	CHECK(!m.halted());
}

TEST(halted_machine, the_cycle_count_reads_as_64_bits_through_a_latched_high_word)
{
	CeresVM vm;
	TimerDevice timer;
	timer.attachTo(vm.io());
	vm.engine().setCycles(0x1'0000'0005ull);    // past 2^32 cycles
	CHECK_EQ(timer.read(TimerDevice::TicksHighRegister), 0u);   // nothing latched yet
	CHECK_EQ(timer.read(TimerDevice::TicksRegister), 5u);
	vm.engine().setCycles(0x2'0000'0004ull);    // the count moves on between the two reads...
	CHECK_EQ(timer.read(TimerDevice::TicksHighRegister), 1u);   // ...and the pair is still one moment
	CHECK_EQ(timer.read(TimerDevice::TicksRegister), 4u);
	CHECK_EQ(timer.read(TimerDevice::TicksHighRegister), 2u);
	timer.detachFrom(vm.io());
}

TEST(halted_machine, the_alarm_arms_on_the_nanosecond_clock_and_disarms_with_zero)
{
	CeresVM vm;
	TimerDevice timer;
	timer.attachTo(vm.io());
	const Scheduler& events = vm.io().scheduler();
	CHECK_EQ(timer.alarmNanos(), u64{ 0 });
	const u64 at = 20'000'000;                  // 20 ms
	timer.write(TimerDevice::AlarmLowRegister, static_cast<u32>(at));
	CHECK_EQ(timer.alarmNanos(), u64{ 0 });     // the low word alone does not arm it
	CHECK_EQ(timer.read(TimerDevice::AlarmLowRegister), 0u);   // and a disarmed alarm reads 0:0
	timer.write(TimerDevice::AlarmHighRegister, static_cast<u32>(at >> 32));
	CHECK_EQ(timer.alarmNanos(), at);
	CHECK_EQ(timer.read(TimerDevice::AlarmLowRegister), static_cast<u32>(at));
	CHECK_EQ(events.cycleOf(timer, TimerDevice::AlarmEvent), at / 20);

	timer.write(TimerDevice::AlarmLowRegister, 0);
	timer.write(TimerDevice::AlarmHighRegister, 0);
	CHECK_EQ(timer.alarmNanos(), u64{ 0 });     // 0:0 disarms
	CHECK(events.empty());                      // and drops its event
	timer.detachFrom(vm.io());
}

TEST(halted_machine, a_masked_halt_jumps_to_the_alarm)
{
	// No handler, interrupts masked: arm the alarm, halt, and the alarm's own request wakes it - in one step.
	HaltedMachine m;
	m.vm().memory().writeUnchecked<u32>(Address(0x404), Instruction::HALT().raw());
	m.vm().engine().setFlags(FlagRegister{});
	m.vm().engine().setProgramCounter(Address(0x404));
	const u64 at = m.timer.nanos() + 30'000'000;   // 30 ms
	m.armAlarm(at);

	m.step();
	CHECK(m.halted());
	m.step();                                   // straight to the alarm's cycle
	CHECK_EQ(m.timer.nanos(), at);              // 30 ms is a whole number of 20 ns cycles
	CHECK_EQ(m.timer.alarmNanos(), u64{ 0 });   // it fired once and disarmed
	CHECK(m.vm().interrupts().peek() == TimerDevice::AlarmInterrupt);
	m.step();
	CHECK(!m.halted());
}

TEST(halted_machine, two_runs_read_the_same_clock)
{
	// The whole point of the machine's own time: the same program reads the same instants on every run.
	const auto run = []
	{
		HaltedMachine m;
		m.timer.arm(12'345);
		m.runToHalt();
		for (int i = 0; i < 4; ++i)
			m.step();
		const u32 low = m.timer.read(TimerDevice::NanosLowRegister);
		return (static_cast<u64>(m.timer.read(TimerDevice::NanosHighRegister)) << 32) | low;
	};
	CHECK_EQ(run(), run());
}

TEST(halted_machine, the_timer_reports_the_cpu_clock)
{
	TimerDevice timer;
	CHECK_EQ(timer.read(TimerDevice::HaltClockRegister), static_cast<u32>(DefaultCpuClockHz));
	CHECK_EQ(timer.clockHz(), DefaultCpuClockHz);
}

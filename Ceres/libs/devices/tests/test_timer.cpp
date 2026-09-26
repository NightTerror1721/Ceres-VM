// The timer (TimerDevice): the device on its own, and a program reaching it through its registers.
#include "device_test_machine.h"

// --- Reading the nanosecond clock ---------------------------------------------------------------

namespace
{
	// Reads the pair the way a program does: the low word first, which latches the high word.
	u64 readNanos(TimerDevice& timer)
	{
		const u32 low = timer.read(TimerDevice::NanosLowRegister);
		const u32 high = timer.read(TimerDevice::NanosHighRegister);
		return (static_cast<u64>(high) << 32) | low;
	}
}

// --- The timer ---------------------------------------------------------------------------------

TEST(timer, the_tick_port_counts_cpu_cycles)
{
	Machine m{
		Instruction::NOP(),
		Instruction::LUI(2, 1),
		Instruction::LDR(3, 2, 0),             // a RAM load: 2 cycles
		Instruction::NOP(),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());

	m.step(4);

	CHECK_EQ(timer.ticks(), u64{ 1 + 1 + 2 + 1 });
	CHECK_EQ(timer.ticks(), m.vm().engine().cycles());
}

TEST(timer, the_countdown_is_counted_in_cycles_not_instructions)
{
	// Two RAM loads and a nop are five cycles: armed for five, the timer fires after the third instruction.
	Machine m{
		Instruction::STI(), Instruction::LUI(2, 1),
		Instruction::LDR(3, 2, 0), Instruction::LDR(3, 2, 0), Instruction::NOP(),
		Instruction::NOP(),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	m.installHandler(TimerDevice::Interrupt, Address(0x800), { Instruction::LI(9, 0xABC), Instruction::IRET() });

	m.step(2);
	timer.arm(5);
	m.step(3);                                  // the loads end on cycles 4 and 6, the nop on 7
	CHECK_EQ(m.vm().engine().cycles(), u64{ 7 });
	CHECK(timer.isArmed());                     // due, and serviced before the next instruction
	m.step(1);                                  // the event runs and its interrupt is taken
	CHECK(!timer.isArmed());
	m.step(1);
	CHECK_EQ(m.reg(9), 0xABCu);
}

TEST(timer, the_countdown_is_an_event_on_the_scheduler)
{
	Machine m{ Instruction::NOP(), Instruction::NOP() };
	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	const Scheduler& events = m.vm().io().scheduler();

	m.step(2);
	timer.arm(10);
	CHECK_EQ(events.cycleOf(timer, TimerDevice::CountdownEvent), u64{ 12 });
	timer.arm(0);
	CHECK_EQ(events.cycleOf(timer, TimerDevice::CountdownEvent), NoScheduledEvent);

	timer.arm(10, true);
	timer.reset();                              // a reset drops what the last program armed
	CHECK(events.empty());

	timer.arm(4);
	timer.detachFrom(m.vm().io());              // and so does unplugging it
	CHECK(events.empty());
}

TEST(timer, a_periodic_timer_keeps_its_period_from_the_cycle_it_was_due)
{
	// Due on cycle 3 but looked at after a load that ends on cycle 4, the next expiry is still on cycle 6, not 7.
	Machine m{ Instruction::LUI(2, 1), Instruction::NOP(), Instruction::LDR(3, 2, 0), Instruction::NOP() };
	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(3, true);
	m.step(4);                                  // cycles 1, 2, 4, and the event runs before the last nop
	CHECK_EQ(m.vm().engine().cycles(), u64{ 5 });
	CHECK_EQ(m.vm().io().scheduler().cycleOf(timer, TimerDevice::CountdownEvent), u64{ 6 });
}

TEST(timer, a_restored_countdown_lands_on_the_cycle_it_would_have)
{
	Machine m{ Instruction::NOP(), Instruction::NOP(), Instruction::NOP(), Instruction::NOP() };
	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(10);
	m.step(3);
	const auto state = timer.captureState();
	CHECK_EQ(state.remaining, u64{ 7 });

	m.step(1);                                  // the live run moves on ...
	timer.reset();
	m.vm().engine().setCycles(3);               // ... and the debugger puts the clock back first, then the timer
	timer.restoreState(state);
	CHECK_EQ(m.vm().io().scheduler().cycleOf(timer, TimerDevice::CountdownEvent), u64{ 10 });
}

TEST(timer, an_armed_timer_raises_its_interrupt)
{
	Machine m{
		Instruction::STI(),
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::NOP(),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(3);

	// A handler that just marks a register and returns.
	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0xABC),
		Instruction::IRET(),
	});

	m.step(8);

	CHECK_EQ(m.reg(9), 0xABCu);
	CHECK(!timer.isArmed());
}

TEST(timer, a_masked_timer_interrupt_stays_pending_until_interrupts_are_enabled)
{
	// Without STI the request must be held, not thrown away: user interrupts are masked while
	// the interrupt flag is clear.
	Machine m{
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::STI(),
		Instruction::NOP(),
		Instruction::NOP(),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(2);

	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x5A),
		Instruction::IRET(),
	});

	// After three steps the timer has expired but the flag is still clear.
	m.step(3);
	CHECK_EQ(m.reg(9), 0u);
	CHECK(m.vm().interrupts().hasPending());

	// STI, and the queued request is delivered.
	m.step(4);
	CHECK_EQ(m.reg(9), 0x5Au);
}

TEST(timer, the_timer_wakes_a_halted_machine)
{
	// This is what the timer is for. HALT used to suspend the machine for good, because no device
	// could ever speak first.
	Machine m{
		Instruction::STI(),
		Instruction::HALT(),
		Instruction::LI(9, 0x77),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(4);

	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::IRET(),
	});

	m.step(2);
	CHECK(m.flags().halting());

	// Stepping on, the timer expires and the interrupt clears the halting flag.
	m.step(12);

	CHECK(!m.flags().halting());
	CHECK_EQ(m.reg(9), 0x77u);
}

TEST(timer, a_periodic_timer_re_arms_itself)
{
	Machine m{
		Instruction::STI(),
		Instruction::NOP(), Instruction::NOP(), Instruction::NOP(), Instruction::NOP(),
		Instruction::NOP(), Instruction::NOP(), Instruction::NOP(), Instruction::NOP(),
		Instruction::NOP(), Instruction::NOP(), Instruction::NOP(), Instruction::NOP(),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(2, true);

	// The handler counts how many times it ran.
	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::ADDI(9, 9, 1),
		Instruction::IRET(),
	});

	m.step(20);

	CHECK(m.reg(9) >= 2u);
	CHECK(timer.isArmed());
}

TEST(timer, a_program_can_arm_the_timer_through_its_register)
{
	Machine m{
		Instruction::STI(),
		Instruction::LI(1, 3),
		LoadBase(TimerBase), LoadBaseLow(TimerBase),
		Instruction::STR(Base, 1, Off(TimerDevice::CommandRegister)),
		Instruction::NOP(), Instruction::NOP(), Instruction::NOP(), Instruction::NOP(),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());

	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x33),
		Instruction::IRET(),
	});

	m.step(14);

	CHECK_EQ(m.reg(9), 0x33u);
}

TEST(timer, writing_zero_disarms_the_timer)
{
	Machine m{ Instruction::NOP() };

	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(5);
	CHECK(timer.isArmed());

	timer.arm(0);
	CHECK(!timer.isArmed());
}

// --- STI takes effect one instruction late ------------------------------------------------------

TEST(timer, sti_then_halt_cannot_lose_an_interrupt_that_arrives_between_them)
{
	// The timer expires on the very step that runs STI. Before, the interrupt was serviced before
	// the HALT ran, the handler returned to the HALT, and the machine slept with nothing left to
	// wake it.
	Machine m{
		Instruction::STI(),
		Instruction::HALT(),
		Instruction::LI(10, 0x33),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(1);

	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x5A),
		Instruction::IRET(),
	});

	m.step(5);

	CHECK_EQ(m.reg(9), 0x5Au);
	CHECK_EQ(m.reg(10), 0x33u); // Woke, and went on past the HALT
	CHECK(!m.flags().halting());
}

TEST(timer, the_instruction_after_sti_runs_before_a_pending_interrupt_is_delivered)
{
	Machine m{
		Instruction::STI(),
		Instruction::LI(1, 1),
		Instruction::LI(2, 2),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(1);

	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x5A),
		Instruction::IRET(),
	});

	m.step(2);
	CHECK_EQ(m.reg(1), 1u);
	CHECK_EQ(m.reg(9), 0u); // Not yet

	m.step(1);
	CHECK_EQ(m.reg(9), 0x5Au); // Delivered on the step after
}

TEST(timer, an_interrupt_already_enabled_is_not_delayed)
{
	// Only the STI itself opens the window: with the flag set already, delivery is immediate.
	Machine m{
		Instruction::STI(),
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::NOP(),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());

	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x5A),
		Instruction::IRET(),
	});

	m.step(3);
	timer.arm(1);
	m.step(2);

	CHECK_EQ(m.reg(9), 0x5Au);
}

// --- The machine's clocks ------------------------------------------------------------------------

TEST(timer, the_clocks_are_the_cycles_at_the_cpu_clock)
{
	CeresVM vm;
	TimerDevice timer{};
	timer.attachTo(vm.io());
	CHECK_EQ(timer.read(TimerDevice::HaltClockRegister), static_cast<u32>(DefaultCpuClockHz));
	CHECK_EQ(timer.read(TimerDevice::NanosResolutionRegister), 20u);   // a cycle at 50 MHz

	vm.engine().setCycles(DefaultCpuClockHz * 3 + 25);                 // three seconds and 25 cycles
	CHECK_EQ(readNanos(timer), u64{ 3'000'000'500 });
	CHECK_EQ(timer.read(TimerDevice::MillisRegister), 3000u);
	CHECK_EQ(timer.nanos(), u64{ 3'000'000'500 });
	timer.detachFrom(vm.io());
}

TEST(timer, the_clocks_stand_still_while_nothing_runs)
{
	// Time is the machine's, not the host's: two reads with no instruction between them are one instant.
	CeresVM vm;
	TimerDevice timer{};
	timer.attachTo(vm.io());
	vm.engine().setCycles(1234);
	const u64 first = readNanos(timer);
	CHECK_EQ(readNanos(timer), first);
	CHECK_EQ(timer.read(TimerDevice::MillisRegister), timer.read(TimerDevice::MillisRegister));
	timer.detachFrom(vm.io());
}

TEST(timer, the_high_word_stays_with_the_low_word_it_was_latched_by)
{
	// A clock that has moved past a carry between the two reads: the high word read now must still
	// be the one that went with the low word, or the pair is 4.29 seconds out.
	CeresVM vm;
	TimerDevice timer{};
	timer.attachTo(vm.io());
	vm.engine().setCycles(429'496'729);                                 // 0x1FFFFFFF4 ns at 20 ns a cycle

	const u32 low = timer.read(TimerDevice::NanosLowRegister);
	vm.engine().setCycles(429'496'730);                                 // the next instant carries into the high word
	const u32 high = timer.read(TimerDevice::NanosHighRegister);

	CHECK_EQ(low, 0xFFFFFFF4u);
	CHECK_EQ(high, 1u);

	// The next low read latches the new instant.
	const u32 nextLow = timer.read(TimerDevice::NanosLowRegister);
	const u32 nextHigh = timer.read(TimerDevice::NanosHighRegister);
	CHECK_EQ(nextLow, 0x8u);
	CHECK_EQ(nextHigh, 2u);
	timer.detachFrom(vm.io());
}

TEST(timer, a_snapshot_of_the_timer_keeps_the_latched_high_word)
{
	// Read the low word, snapshot, and read the high word from another timer: the debugger does
	// exactly this when it puts a machine back between the two reads of a pair.
	CeresVM vm;
	TimerDevice timer{};
	timer.attachTo(vm.io());
	vm.engine().setCycles(DefaultCpuClockHz * 5);                       // 5 s: the high word is 1
	timer.read(TimerDevice::NanosLowRegister);
	const auto state = timer.captureState();
	timer.detachFrom(vm.io());

	TimerDevice other{};
	other.restoreState(state);
	CHECK_EQ(other.read(TimerDevice::NanosHighRegister), 1u);
}

TEST(timer, the_alarm_is_scheduled_on_the_first_cycle_at_or_after_its_instant)
{
	CeresVM vm;
	TimerDevice timer{};
	timer.attachTo(vm.io());
	const Scheduler& events = vm.io().scheduler();

	timer.write(TimerDevice::AlarmLowRegister, 1000);
	timer.write(TimerDevice::AlarmHighRegister, 0);
	CHECK_EQ(events.cycleOf(timer, TimerDevice::AlarmEvent), u64{ 50 });
	timer.write(TimerDevice::AlarmLowRegister, 1010);                   // between two cycles: the later one
	timer.write(TimerDevice::AlarmHighRegister, 0);
	CHECK_EQ(events.cycleOf(timer, TimerDevice::AlarmEvent), u64{ 51 });

	vm.engine().setCycles(100);                                         // an instant already past fires at once
	timer.write(TimerDevice::AlarmLowRegister, 1000);
	timer.write(TimerDevice::AlarmHighRegister, 0);
	CHECK_EQ(timer.alarmNanos(), u64{ 0 });
	CHECK(vm.interrupts().hasPending());
	CHECK(events.empty());
	timer.detachFrom(vm.io());
}

TEST(timer, writing_to_the_nanosecond_registers_changes_nothing)
{
	TimerDevice timer{};
	timer.write(TimerDevice::NanosLowRegister, 5);
	timer.write(TimerDevice::NanosHighRegister, 5);
	timer.write(TimerDevice::NanosResolutionRegister, 5);
	CHECK(!timer.isArmed());
	CHECK_EQ(readNanos(timer), u64{ 0 });                               // not attached: no clock at all
	CHECK_EQ(timer.read(TimerDevice::NanosResolutionRegister), 20u);
}

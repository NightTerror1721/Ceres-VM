// The timer (TimerDevice): the device on its own, and a program reaching it through its registers.
#include "device_test_machine.h"

// --- A nanosecond clock ------------------------------------------------------------------------

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

TEST(timer, the_tick_port_counts_executed_instructions)
{
	Machine m{
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::NOP(),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());

	m.step(3);

	CHECK_EQ(timer.ticks(), u64{ 3 });
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

// --- A millisecond clock -----------------------------------------------------------------------

TEST(timer, the_millisecond_register_never_goes_backwards)
{
	TimerDevice timer{};
	const u32 first = timer.read(TimerDevice::MillisRegister);
	const u32 second = timer.read(TimerDevice::MillisRegister);
	CHECK(second >= first);
	CHECK(first < 60000u); // Counts from the machine's start, not the epoch
}

TEST(timer, the_millisecond_clock_can_be_replaced_for_a_replay)
{
	TimerDevice timer{};
	timer.setMillisSource([] { return u32{ 1234 }; });
	CHECK_EQ(timer.read(TimerDevice::MillisRegister), 1234u);

	timer.clearMillisSource();
	CHECK(timer.read(TimerDevice::MillisRegister) != 1234u);
}

TEST(timer, the_nanosecond_pair_counts_from_the_start_and_never_goes_backwards)
{
	TimerDevice timer{};
	u64 last = readNanos(timer);
	CHECK(last < 4000000000ull); // Under four seconds after the timer was made, so the high word is still 0

	for (int i = 0; i < 1000; ++i)
	{
		const u64 now = readNanos(timer);
		CHECK(now >= last);
		last = now;
	}
}

TEST(timer, the_high_word_stays_with_the_low_word_it_was_latched_by)
{
	// A clock that has moved past a carry between the two reads: the high word read now must still
	// be the one that went with the low word, or the pair is 4.29 seconds out.
	u64 now = 0x00000001FFFFFFF0ull;
	TimerDevice timer{};
	timer.setNanosSource([&now] { return now; });

	const u32 low = timer.read(TimerDevice::NanosLowRegister);
	now = 0x0000000200000010ull; // The next instant carries into the high word
	const u32 high = timer.read(TimerDevice::NanosHighRegister);

	CHECK_EQ(low, 0xFFFFFFF0u);
	CHECK_EQ(high, 1u);

	// The next low read latches the new instant.
	const u32 nextLow = timer.read(TimerDevice::NanosLowRegister);
	const u32 nextHigh = timer.read(TimerDevice::NanosHighRegister);
	CHECK_EQ(nextLow, 0x10u);
	CHECK_EQ(nextHigh, 2u);
}

TEST(timer, reading_the_high_word_alone_does_not_look_at_the_clock)
{
	int asked = 0;
	TimerDevice timer{};
	timer.setNanosSource([&asked] { ++asked; return u64{ 7 }; });

	CHECK_EQ(timer.read(TimerDevice::NanosHighRegister), 0u); // Nothing latched yet
	CHECK_EQ(asked, 0);

	timer.read(TimerDevice::NanosLowRegister);
	CHECK_EQ(asked, 1);
	timer.read(TimerDevice::NanosHighRegister);
	CHECK_EQ(asked, 1);
}

TEST(timer, a_snapshot_of_the_timer_keeps_the_latched_high_word)
{
	// Read the low word, snapshot, and read the high word from another timer: the debugger does
	// exactly this when it puts a machine back between the two reads of a pair.
	TimerDevice timer{};
	timer.setNanosSource([] { return 0x0000000500000009ull; });
	timer.read(TimerDevice::NanosLowRegister);
	const auto state = timer.captureState();

	TimerDevice other{};
	other.restoreState(state);
	CHECK_EQ(other.read(TimerDevice::NanosHighRegister), 5u);
}

TEST(timer, the_resolution_is_a_real_step_of_the_host_clock)
{
	TimerDevice timer{};
	const u32 resolution = timer.read(TimerDevice::NanosResolutionRegister);
	CHECK(resolution >= 1u);
	CHECK(resolution <= 1000000u); // A clock coarser than a millisecond would not be worth a nanosecond register
	CHECK_EQ(timer.read(TimerDevice::NanosResolutionRegister), resolution); // Measured once

	timer.setNanosResolution(100);
	CHECK_EQ(timer.read(TimerDevice::NanosResolutionRegister), 100u);
}

TEST(timer, writing_to_the_nanosecond_registers_changes_nothing)
{
	TimerDevice timer{};
	timer.write(TimerDevice::NanosLowRegister, 5);
	timer.write(TimerDevice::NanosHighRegister, 5);
	timer.write(TimerDevice::NanosResolutionRegister, 5);
	CHECK(!timer.isArmed());
	CHECK(readNanos(timer) < 4000000000ull);
}

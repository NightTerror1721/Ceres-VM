// The terminal (TerminalDevice): the device on its own, and a program reaching it through its registers.
#include "device_test_machine.h"

// --- The terminal's input interrupt -------------------------------------------------------------

TEST(terminal, pushing_input_raises_the_terminals_interrupt)
{
	Machine m{ Instruction::STI(), Instruction::NOP(), Instruction::NOP() };

	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());

	m.installHandler(TerminalDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x51),
		Instruction::IRET(),
	});

	terminal.pushInput('X');
	m.step(3);

	CHECK_EQ(m.reg(9), 0x51u);
}

TEST(terminal, the_terminal_wakes_a_halted_machine_on_input)
{
	// The motivating case: HALT suspends the machine until *some* interrupt arrives, and the
	// terminal is now one of the things that can raise one - not just the timer.
	Machine m{
		Instruction::STI(),
		Instruction::HALT(),
		LoadBase(TerminalBase), LoadBaseLow(TerminalBase),
		Instruction::LDR(1, Base, Off(TerminalDevice::InputRegister)),
	};

	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());

	m.installHandler(TerminalDevice::Interrupt, Address(0x800), {
		Instruction::IRET(),
	});

	m.step(2);
	CHECK(m.flags().halting());

	// Nothing was pending before this: the interrupt is raised by the push itself, not by time
	// passing, which is exactly the difference from the timer.
	terminal.pushInput('X');
	m.step(4);

	CHECK(!m.flags().halting());
	CHECK_EQ(m.reg(1), static_cast<u32>('X'));
}

TEST(terminal, pushing_input_with_nothing_to_deliver_raises_no_interrupt)
{
	// An empty span (or a full ring buffer) writes no byte, so there is nothing to be woken up
	// about - and nothing here to distinguish from the interrupt never having been requested.
	Machine m{ Instruction::STI(), Instruction::HALT() };

	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());

	m.step(2);
	CHECK(m.flags().halting());

	terminal.pushInput(std::span<const u8>{});
	m.step(1);

	CHECK(m.flags().halting());
}

TEST(terminal, terminal_counts_input_discarded_by_a_full_buffer)
{
	TerminalDevice terminal{};
	std::vector<u8> input(TerminalDevice::InputBufferCapacity + 8, 'X');

	terminal.pushInput(input);

	CHECK_EQ(terminal.droppedInputBytes(), u64{9});
}

TEST(terminal, the_terminal_reports_how_many_bytes_are_available)
{
	TerminalDevice terminal{};

	CHECK_EQ(terminal.availableBytes(), usize{ 0 });

	terminal.pushInput("AB");
	CHECK_EQ(terminal.availableBytes(), usize{ 2 });
	CHECK_EQ(terminal.read(TerminalDevice::BytesAvailableRegister), u32{ 2 });

	terminal.read(TerminalDevice::InputRegister);
	CHECK_EQ(terminal.read(TerminalDevice::BytesAvailableRegister), u32{ 1 });
}

TEST(terminal, a_block_read_reports_how_many_bytes_it_actually_moved)
{
	// Five bytes buffered, ten asked for: the read is short, and the count register is the only
	// way the program can tell where its input ended.
	CeresVM vm{};
	TerminalDevice terminal{};
	terminal.attachTo(vm.io());
	terminal.pushInput("ABCDE");

	terminal.write(TerminalDevice::BlockAddressRegister, 0x1000);
	terminal.write(TerminalDevice::BlockLengthRegister, 10);
	terminal.write(TerminalDevice::BlockCommandRegister, TerminalDevice::BlockCommandRead);

	CHECK_EQ(terminal.read(TerminalDevice::BlockReadCountRegister), u32{ 5 });
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(0x1000)), u8{ 'A' });
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(0x1004)), u8{ 'E' });
	CHECK_EQ(terminal.availableBytes(), usize{ 0 });
}

TEST(terminal, a_zero_length_block_read_reports_nothing_moved)
{
	CeresVM vm{};
	TerminalDevice terminal{};
	terminal.attachTo(vm.io());
	terminal.pushInput("X");

	terminal.write(TerminalDevice::BlockAddressRegister, 0x1000);
	terminal.write(TerminalDevice::BlockLengthRegister, 0);
	terminal.write(TerminalDevice::BlockCommandRegister, TerminalDevice::BlockCommandRead);

	CHECK_EQ(terminal.read(TerminalDevice::BlockReadCountRegister), u32{ 0 });
	CHECK_EQ(terminal.availableBytes(), usize{ 1 }); // Nothing was consumed.
}

TEST(terminal, a_block_read_to_an_out_of_range_address_moves_nothing_instead_of_throwing)
{
	// A program-supplied BLOCK_ADDR past the end of RAM must not crash the machine: the read is
	// simply empty, and the count register says so.
	CeresVM vm{};
	TerminalDevice terminal{};
	terminal.attachTo(vm.io());
	terminal.pushInput("ABCDE");

	terminal.write(TerminalDevice::BlockAddressRegister, 0xF0000000u);
	terminal.write(TerminalDevice::BlockLengthRegister, 5);
	terminal.write(TerminalDevice::BlockCommandRegister, TerminalDevice::BlockCommandRead);

	CHECK_EQ(terminal.read(TerminalDevice::BlockReadCountRegister), u32{ 0 });
	CHECK_EQ(terminal.availableBytes(), usize{ 5 }); // Nothing was consumed.
}

TEST(terminal, a_block_read_is_clamped_to_the_end_of_ram)
{
	// Four bytes of RAM left, ten bytes buffered: the transfer stops at the end of memory and the
	// count reports the four it actually moved.
	CeresVM vm{}; // 16 MiB default
	TerminalDevice terminal{};
	terminal.attachTo(vm.io());
	terminal.pushInput("ABCDEFGHIJ");

	const u32 lastWord = static_cast<u32>(vm.memory().size()) - 4u;
	terminal.write(TerminalDevice::BlockAddressRegister, lastWord);
	terminal.write(TerminalDevice::BlockLengthRegister, 100);
	terminal.write(TerminalDevice::BlockCommandRegister, TerminalDevice::BlockCommandRead);

	CHECK_EQ(terminal.read(TerminalDevice::BlockReadCountRegister), u32{ 4 });
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(lastWord)), u8{ 'A' });
	CHECK_EQ(terminal.availableBytes(), usize{ 6 });
}

TEST(terminal, the_terminal_reports_dropped_input_bytes_to_the_program)
{
	TerminalDevice terminal{};
	std::vector<u8> input(TerminalDevice::InputBufferCapacity + 8, 'X');

	terminal.pushInput(input);

	CHECK_EQ(terminal.read(TerminalDevice::DroppedInputRegister), u32{ 9 });
}

TEST(terminal, terminal_snapshot_and_restore_preserve_unread_input)
{
	TerminalDevice terminal{};
	terminal.pushInput("AB");

	const auto snapshot = terminal.captureState();
	CHECK_EQ(terminal.read(TerminalDevice::InputRegister), u32{ 'A' });

	terminal.restoreState(snapshot);
	CHECK_EQ(terminal.read(TerminalDevice::InputRegister), u32{ 'A' });
	CHECK_EQ(terminal.read(TerminalDevice::InputRegister), u32{ 'B' });
}

// --- The terminal's output sink ----------------------------------------------------------------

TEST(terminal, terminal_output_goes_to_the_installed_sink_instead_of_stdout)
{
	// Two word writes: each sends its low byte, and only that - the rest of the word is not more text.
	Machine m{
		LoadBase(TerminalBase), LoadBaseLow(TerminalBase),
		Instruction::LI(1, 'C'),
		Instruction::STR(Base, 1, Off(TerminalDevice::OutputRegister)),
		Instruction::LI(2, 0x0000'6165), // 'e' in the low byte, 'a' above it
		Instruction::STR(Base, 2, Off(TerminalDevice::OutputRegister)),
	};

	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());

	std::string captured;
	terminal.setOutputSink([&captured](u8 byte) { captured.push_back(static_cast<char>(byte)); });

	m.step(6);

	CHECK_EQ(captured, std::string{ "Ce" });
}

TEST(terminal, a_multi_byte_character_reaches_the_sink_one_byte_at_a_time)
{
	// 'á' is 0xC3 0xA1 in UTF-8. The device hands over bytes, not characters: a consumer that
	// decoded each one on its own would produce two replacement characters instead of one letter.
	Machine m{
		LoadBase(TerminalBase), LoadBaseLow(TerminalBase),
		Instruction::LI(1, 0xC3),
		Instruction::STR(Base, 1, Off(TerminalDevice::OutputRegister)),
		Instruction::LI(1, 0xA1),
		Instruction::STR(Base, 1, Off(TerminalDevice::OutputRegister)),
	};

	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());

	std::vector<u8> captured;
	terminal.setOutputSink([&captured](u8 byte) { captured.push_back(byte); });

	m.step(6);

	CHECK_EQ(captured.size(), usize{ 2 });
	if (captured.size() == 2)
	{
		CHECK_EQ(captured[0], u8{ 0xC3 });
		CHECK_EQ(captured[1], u8{ 0xA1 });
	}
}

// --- The terminal reports the end of its input --------------------------------------------------

TEST(terminal, an_open_terminal_never_reports_the_end_of_input)
{
	TerminalDevice terminal{};
	CHECK_EQ(terminal.read(TerminalDevice::StatusRegister) & TerminalDevice::StatusEndOfInput, 0u);
	CHECK(!terminal.isInputClosed());
}

TEST(terminal, a_closed_terminal_reports_the_end_only_once_its_input_is_drained)
{
	TerminalDevice terminal{};
	terminal.pushInput("ab");
	terminal.closeInput();

	// Data first: the program still has bytes to read, so this is not the end yet.
	u32 status = terminal.read(TerminalDevice::StatusRegister);
	CHECK_EQ(status & TerminalDevice::StatusInputAvailable, TerminalDevice::StatusInputAvailable);
	CHECK_EQ(status & TerminalDevice::StatusEndOfInput, 0u);

	CHECK_EQ(terminal.read(TerminalDevice::InputRegister), u32{ 'a' });
	CHECK_EQ(terminal.read(TerminalDevice::InputRegister), u32{ 'b' });

	status = terminal.read(TerminalDevice::StatusRegister);
	CHECK_EQ(status & TerminalDevice::StatusInputAvailable, 0u);
	CHECK_EQ(status & TerminalDevice::StatusEndOfInput, TerminalDevice::StatusEndOfInput);
	// Output stays ready: closing the input says nothing about the screen.
	CHECK_EQ(status & TerminalDevice::StatusOutputReady, TerminalDevice::StatusOutputReady);
}

TEST(terminal, closing_the_input_wakes_a_halted_machine)
{
	Machine m{ Instruction::STI(), Instruction::HALT(), Instruction::LI(10, 0x21) };

	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());
	m.installHandler(TerminalDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x99),
		Instruction::IRET(),
	});

	m.step(2);
	CHECK(m.flags().halting());

	terminal.closeInput();
	m.step(4);

	CHECK_EQ(m.reg(9), 0x99u);
	CHECK_EQ(m.reg(10), 0x21u);
}

TEST(terminal, a_snapshot_of_the_terminal_remembers_that_its_input_was_closed)
{
	TerminalDevice terminal{};
	terminal.closeInput();
	const auto state = terminal.captureState();
	CHECK(state.closed);

	TerminalDevice other{};
	other.restoreState(state);
	CHECK(other.isInputClosed());
}

// --- The terminal's raw-keys request -----------------------------------------------------------

TEST(terminal, a_terminal_with_no_host_behind_it_grants_no_raw_keys)
{
	TerminalDevice terminal{};
	terminal.write(TerminalDevice::ModeRegister, TerminalDevice::ModeRaw);
	CHECK(terminal.rawRequested());
	CHECK_EQ(terminal.read(TerminalDevice::ModeRegister), 0u);   // asked, and not given
}

TEST(terminal, the_host_decides_what_a_raw_request_is_granted)
{
	TerminalDevice terminal{};
	u32 asked = 99;
	terminal.setModeHandler([&](u32 requested)
	{
		asked = requested;
		return requested ? (TerminalDevice::ModeRaw | TerminalDevice::ModeKeystrokes) : 0u;
	});

	terminal.write(TerminalDevice::ModeRegister, TerminalDevice::ModeRaw);
	CHECK_EQ(asked, TerminalDevice::ModeRaw);
	CHECK_EQ(terminal.read(TerminalDevice::ModeRegister), TerminalDevice::ModeRaw | TerminalDevice::ModeKeystrokes);
	CHECK(terminal.rawRequested());

	terminal.write(TerminalDevice::ModeRegister, 0);
	CHECK_EQ(asked, 0u);
	CHECK_EQ(terminal.read(TerminalDevice::ModeRegister), 0u);
	CHECK(!terminal.rawRequested());
}

TEST(terminal, only_the_raw_bit_of_a_mode_write_is_a_request)
{
	TerminalDevice terminal{};
	u32 asked = 99;
	terminal.setModeHandler([&](u32 requested) { asked = requested; return requested; });
	terminal.write(TerminalDevice::ModeRegister, 0xFFFFFFFEu);   // the keystrokes bit is the host's to set
	CHECK_EQ(asked, 0u);
}

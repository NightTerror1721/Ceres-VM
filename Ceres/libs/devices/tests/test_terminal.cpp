// The virtual terminal (TerminalDevice, plan/v2 SPEC 8): its streams, its input, and what it draws in the text plane.
#include "device_test_machine.h"

#include <ceres/devices/video/gpu.h>

namespace
{
	// A machine with the GPU and the terminal drawing in it, the screen made small so the tests can see all of it.
	struct Screen
	{
		CeresVM vm;
		GpuDevice gpu;
		TerminalDevice terminal;
		std::string out;
		std::string err;

		Screen(u32 cols = 10, u32 rows = 4)
		{
			gpu.attachTo(vm.io());
			vm.io().write(default_mmio::Gpu + GpuDevice::WidthRegister, cols * 8);
			vm.io().write(default_mmio::Gpu + GpuDevice::HeightRegister, rows * 16);
			terminal.attachTo(vm.io());
			terminal.setScreen(&gpu);
			terminal.setOutputSink([this](u8 byte) { out.push_back(static_cast<char>(byte)); });
			terminal.setErrorSink([this](u8 byte) { err.push_back(static_cast<char>(byte)); });
		}
		~Screen()
		{
			terminal.detachFrom(vm.io());
			gpu.detachFrom(vm.io());
		}

		void write(std::string_view text, bool error = false)
		{
			for (char c : text)
				terminal.write(error ? TerminalDevice::ErrorOutputRegister : TerminalDevice::OutputRegister, static_cast<u8>(c));
		}
		std::string text() const { return gpu.screenText(); }
		u16 cell(u32 x, u32 y) { return vm.vram().read<u16>(gpu.textPlane().cellAddress(x, y) - Vram::BaseValue); }
		u32 reg(Address offset) { return terminal.read(offset); }
		// What the program reads, as it would: the status, then a byte while there is one. Reading is what takes
		// in what a script typed.
		std::string readInput()
		{
			std::string in;
			while (terminal.read(TerminalDevice::StatusRegister) & TerminalDevice::StatusInputAvailable)
				in.push_back(static_cast<char>(terminal.read(TerminalDevice::InputRegister)));
			return in;
		}
	};
}

// --- The input interrupt and the input queue ------------------------------------------------------

TEST(terminal, pushing_input_raises_the_terminals_interrupt)
{
	Machine m{ Instruction::STI(), Instruction::NOP(), Instruction::NOP() };
	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());
	m.installHandler(TerminalDevice::Interrupt, Address(0x800), { Instruction::LI(9, 0x51), Instruction::IRET() });

	terminal.pushInput('X');
	m.step(3);
	CHECK_EQ(m.reg(9), 0x51u);
	terminal.detachFrom(m.vm().io());
}

TEST(terminal, the_terminal_wakes_a_halted_machine_on_input)
{
	Machine m{
		Instruction::STI(),
		Instruction::HALT(),
		LoadBase(TerminalBase), LoadBaseLow(TerminalBase),
		Instruction::LDR(1, Base, Off(TerminalDevice::InputRegister)),
	};
	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());
	m.installHandler(TerminalDevice::Interrupt, Address(0x800), { Instruction::IRET() });

	m.step(2);
	CHECK(m.flags().halting());
	terminal.pushInput('X');
	m.step(4);
	CHECK(!m.flags().halting());
	CHECK_EQ(m.reg(1), static_cast<u32>('X'));
	terminal.detachFrom(m.vm().io());
}

TEST(terminal, without_mode_bit_3_input_raises_no_interrupt)
{
	CeresVM vm;
	TerminalDevice terminal{};
	terminal.attachTo(vm.io());
	terminal.write(TerminalDevice::ModeRegister, TerminalDevice::ModeEcho);
	terminal.pushInput('X');
	CHECK(!vm.interrupts().hasPending());
	CHECK_EQ(terminal.read(TerminalDevice::AvailableRegister), 1u);
	terminal.detachFrom(vm.io());
}

TEST(terminal, input_past_the_capacity_is_dropped_and_counted)
{
	TerminalDevice terminal{};
	const std::string lots(TerminalDevice::InputBufferCapacity + 5, 'a');
	terminal.pushInput(lots);
	CHECK_EQ(terminal.availableBytes(), TerminalDevice::InputBufferCapacity);
	CHECK_EQ(terminal.droppedInputBytes(), u64{ 5 });
	CHECK_EQ(terminal.inputRoom(), usize{ 0 });
}

TEST(terminal, a_block_read_moves_what_is_waiting_and_says_how_much)
{
	Machine m{ Instruction::NOP() };
	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());
	terminal.pushInput("abc");
	terminal.write(TerminalDevice::BlockAddressRegister, DestinationBuffer);
	terminal.write(TerminalDevice::BlockLengthRegister, 16);
	terminal.write(TerminalDevice::BlockCommandRegister, TerminalDevice::BlockCommandRead);
	CHECK_EQ(terminal.read(TerminalDevice::BlockCountRegister), 3u);
	CHECK_EQ(readBack(m.memory(), DestinationBuffer, 3), std::string("abc"));
	CHECK_EQ(terminal.availableBytes(), usize{ 0 });

	// An address past the RAM moves nothing, and does not throw.
	terminal.pushInput("z");
	terminal.write(TerminalDevice::BlockAddressRegister, 0x7FFFFFF0);
	terminal.write(TerminalDevice::BlockCommandRegister, TerminalDevice::BlockCommandRead);
	CHECK_EQ(terminal.read(TerminalDevice::BlockCountRegister), 0u);
	terminal.detachFrom(m.vm().io());
}

TEST(terminal, a_block_write_goes_to_the_stream_it_names)
{
	Machine m{ Instruction::NOP() };
	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());
	std::string out, err;
	terminal.setOutputSink([&](u8 byte) { out.push_back(static_cast<char>(byte)); });
	terminal.setErrorSink([&](u8 byte) { err.push_back(static_cast<char>(byte)); });
	fill(m.memory(), SourceBuffer, "hello");
	terminal.write(TerminalDevice::BlockAddressRegister, SourceBuffer);
	terminal.write(TerminalDevice::BlockLengthRegister, 5);
	terminal.write(TerminalDevice::BlockCommandRegister, TerminalDevice::BlockCommandWrite);
	terminal.write(TerminalDevice::BlockCommandRegister, TerminalDevice::BlockCommandWriteError);
	CHECK_EQ(out, std::string("hello"));
	CHECK_EQ(err, std::string("hello"));
	CHECK_EQ(terminal.read(TerminalDevice::BlockCountRegister), 5u);
	terminal.detachFrom(m.vm().io());
}

TEST(terminal, the_end_of_input_shows_only_once_the_input_is_drained)
{
	TerminalDevice terminal{};
	CHECK_EQ(terminal.read(TerminalDevice::StatusRegister) & TerminalDevice::StatusEndOfInput, 0u);
	terminal.pushInput("x");
	terminal.closeInput();
	CHECK_EQ(terminal.read(TerminalDevice::StatusRegister), TerminalDevice::StatusInputAvailable | TerminalDevice::StatusOutputReady);
	CHECK_EQ(terminal.read(TerminalDevice::InputRegister), u32{ 'x' });
	CHECK_EQ(terminal.read(TerminalDevice::StatusRegister), TerminalDevice::StatusEndOfInput | TerminalDevice::StatusOutputReady);
	CHECK(terminal.isInputClosed());
}

TEST(terminal, a_snapshot_restores_the_unread_input_and_the_end)
{
	TerminalDevice terminal{};
	terminal.pushInput("ab");
	terminal.closeInput();
	const TerminalDevice::State saved = terminal.captureState();
	terminal.read(TerminalDevice::InputRegister);
	terminal.read(TerminalDevice::InputRegister);
	terminal.restoreState(saved);
	CHECK_EQ(terminal.availableBytes(), usize{ 2 });
	CHECK(terminal.isInputClosed());
}

TEST(terminal, the_streams_reach_their_sinks_and_nothing_reaches_the_host)
{
	Screen s;
	s.write("o\xC3\xA9");
	s.write("e", true);
	CHECK_EQ(s.out, std::string("o\xC3\xA9"));   // byte by byte, as written
	CHECK_EQ(s.err, std::string("e"));
}

// --- The screen (plan/v2 SPEC 8.2) ----------------------------------------------------------------

TEST(terminal, text_is_drawn_in_the_text_plane_and_the_cursor_follows)
{
	Screen s;
	s.write("Hi\r\nthere");
	CHECK_EQ(s.text(), std::string("Hi\nthere\n\n\n"));
	CHECK_EQ(s.reg(TerminalDevice::CursorXRegister), 5u);
	CHECK_EQ(s.reg(TerminalDevice::CursorYRegister), 1u);
	CHECK_EQ(s.gpu.textPlane().cursorX(), 5u);
	CHECK_EQ(s.gpu.textPlane().cursorShape() & 3u, video::TextPlane::CursorUnderline);
	CHECK_EQ(s.reg(TerminalDevice::ColsRegister), 10u);
	CHECK_EQ(s.reg(TerminalDevice::RowsRegister), 4u);
}

TEST(terminal, a_newline_goes_to_the_start_of_the_next_line)
{
	Screen s;
	s.write("ab\ncd");
	CHECK_EQ(s.text(), std::string("ab\ncd\n\n\n"));
}

TEST(terminal, utf8_is_drawn_as_its_glyph_and_a_character_without_one_as_a_question_mark)
{
	Screen s;
	s.write("\xC3\xA9\xE2\x94\x80\xE4\xB8\xAD");   // é, ─, 中
	CHECK_EQ(s.cell(0, 0) & 0xFF, 0xE9);
	CHECK_EQ(s.cell(1, 0) & 0xFF, 0x80);
	CHECK_EQ(s.cell(2, 0) & 0xFF, u32{ '?' });
}

TEST(terminal, tabs_backspace_and_carriage_return_move_the_cursor)
{
	Screen s;
	s.write("a\tb");
	CHECK_EQ(s.cell(8, 0) & 0xFF, u32{ 'b' });
	s.write("\rX\bY");
	CHECK_EQ(s.text().substr(0, 10), std::string("Y       b\n"));
}

TEST(terminal, a_full_line_wraps_only_when_the_next_character_comes)
{
	Screen s;
	s.write("0123456789");
	CHECK_EQ(s.reg(TerminalDevice::CursorYRegister), 0u);   // the cursor waits at the last column
	s.write("\n");
	CHECK_EQ(s.reg(TerminalDevice::CursorYRegister), 1u);   // one line down, not two
	s.write("0123456789x");
	CHECK_EQ(s.text(), std::string("0123456789\n0123456789\nx\n\n"));
}

TEST(terminal, the_screen_scrolls_and_the_lost_rows_go_to_the_scrollback)
{
	Screen s;
	s.write("1\n2\n3\n4\n5\n6");
	CHECK_EQ(s.text(), std::string("3\n4\n5\n6\n"));
	CHECK_EQ(s.gpu.textPlane().scrollbackCount(), 2u);
	// Shift+PageUp shows them, and output brings the view back.
	s.terminal.typeKeystroke(KeyboardDevice::KeyNamed | KeyboardDevice::KeyShift | scancode::PageUp);
	CHECK_EQ(s.gpu.textPlane().scrollY(), 2u);
	CHECK_EQ(s.gpu.textPlane().shownCell(s.vm.vram(), 0, 0) & 0xFF, u32{ '1' });
	s.write("!");
	CHECK_EQ(s.gpu.textPlane().scrollY(), 0u);
}

TEST(terminal, cursor_and_erase_sequences)
{
	Screen s;
	s.write("abcdefghij\x1b[2;3H*");
	CHECK_EQ(s.cell(2, 1) & 0xFF, u32{ '*' });
	s.write("\x1b[A\x1b[2D+");
	CHECK_EQ(s.cell(1, 0) & 0xFF, u32{ '+' });
	s.write("\x1b[1;5H\x1b[K");
	CHECK_EQ(s.text().substr(0, 5), std::string("a+cd\n"));
	s.write("\x1b[2J");
	CHECK_EQ(s.text(), std::string("\n\n\n\n"));
	s.write("\x1b[3;3Hq\x1b[1J");
	CHECK_EQ(s.text(), std::string("\n\n\n\n"));
}

TEST(terminal, save_and_restore_the_cursor_both_ways)
{
	Screen s;
	s.write("ab\x1b[s\x1b[3;1Hx\x1b[uc\x1b" "7\x1b[4;1H\x1b" "8d");
	CHECK_EQ(s.text(), std::string("abcd\n\nx\n\n"));
}

TEST(terminal, colours_follow_sgr_and_the_error_stream_has_its_own)
{
	Screen s;
	s.write("\x1b[31;44mA\x1b[1mB\x1b[7mC\x1b[0mD\x1b[93;101mE\x1b[38;5;196mF");
	CHECK_EQ(s.cell(0, 0) >> 8, 0x41u);   // red on blue
	CHECK_EQ(s.cell(1, 0) >> 8, 0x49u);   // bold: bright red
	CHECK_EQ(s.cell(2, 0) >> 8, 0x94u);   // reversed
	CHECK_EQ(s.cell(3, 0) >> 8, 0x07u);   // back to the default
	CHECK_EQ(s.cell(4, 0) >> 8, 0x9Bu);   // bright yellow on bright red
	CHECK_EQ(s.cell(5, 0) >> 8, 0x91u);   // 196, the cube's pure red, is nearest to red (1); the bright red background stays
	s.write("\r\n");
	s.write("e", true);
	CHECK_EQ(s.cell(0, 1) >> 8, TerminalDevice::ErrorInk);
	s.write("\x1b[32me\x1b[0mf", true);
	CHECK_EQ(s.cell(1, 1) >> 8, 0x02u);
	CHECK_EQ(s.cell(2, 1) >> 8, TerminalDevice::ErrorInk);   // SGR 0 on the error stream is its own colour
}

TEST(terminal, the_cursor_hides_and_shows_and_a_scroll_region_keeps_the_rest_still)
{
	Screen s;
	s.write("\x1b[?25l");
	CHECK_EQ(s.gpu.textPlane().cursorShape(), video::TextPlane::CursorNone);
	s.write("\x1b[?25h");
	CHECK(s.gpu.textPlane().cursorShape() != video::TextPlane::CursorNone);
	s.write("top\x1b[2;3r\x1b[2;1Ha\nb\nc");
	CHECK_EQ(s.text(), std::string("top\nb\nc\n\n"));
	CHECK_EQ(s.gpu.textPlane().scrollbackCount(), 0u);   // a region's rows do not go to the scrollback
}

TEST(terminal, unknown_sequences_are_consumed_without_effect)
{
	Screen s;
	s.write("a\x1b[5nb\x1b]0;title\x07" "c\x1b(Bd\x1b[?1049he");
	CHECK_EQ(s.text().substr(0, 6), std::string("abcde\n"));
}

// --- The input through the line discipline (plan/v2 SPEC 8.3) --------------------------------------

TEST(terminal, a_typed_line_is_echoed_and_handed_over_on_enter)
{
	Screen s;
	s.terminal.typeKeystroke('h');
	s.terminal.typeKeystroke('x');
	s.terminal.typeKeystroke(0x08);
	s.terminal.typeKeystroke('i');
	CHECK_EQ(s.terminal.availableBytes(), usize{ 0 });
	CHECK_EQ(s.text().substr(0, 3), std::string("hi\n"));   // edited on the screen as it is typed
	s.terminal.typeKeystroke('\n');
	CHECK_EQ(s.readInput(), std::string("hi\n"));
	CHECK(s.out.empty());   // the echo is the terminal's, not the program's output
	CHECK_EQ(s.reg(TerminalDevice::CursorYRegister), 1u);
}

TEST(terminal, a_script_types_as_the_program_reads_under_the_mode_it_has_then)
{
	Screen s;
	s.terminal.type("ab\n\x1b[Aq");
	CHECK_EQ(s.text(), std::string("\n\n\n\n"));        // nothing yet: the program has not looked
	std::string line;                                      // a line, edited and echoed as it is read - and no more
	while (line.empty() || line.back() != '\n')
		if (s.reg(TerminalDevice::StatusRegister) & TerminalDevice::StatusInputAvailable)
			line.push_back(static_cast<char>(s.reg(TerminalDevice::InputRegister)));
	CHECK_EQ(line, std::string("ab\n"));
	CHECK_EQ(s.text().substr(0, 3), std::string("ab\n"));
	// The program goes raw: the arrow the script typed next reaches it as its bytes, not the history.
	s.terminal.write(TerminalDevice::ModeRegister, TerminalDevice::ModeRaw);
	CHECK_EQ(s.readInput(), std::string("\x1b[Aq"));
	// A keystroke from the window waits behind a script's.
	s.terminal.type("1");
	s.terminal.typeKeystroke('2');
	CHECK_EQ(s.readInput(), std::string("12"));
}

TEST(terminal, raw_mode_hands_every_key_over_at_once_without_echo)
{
	Screen s;
	s.terminal.write(TerminalDevice::ModeRegister, TerminalDevice::ModeRaw | TerminalDevice::ModeInterrupt);
	s.terminal.type("q");
	s.terminal.typeKeystroke(KeyboardDevice::KeyNamed | scancode::Up);
	CHECK_EQ(s.readInput(), std::string("q\x1b[A"));
	CHECK_EQ(s.text(), std::string("\n\n\n\n"));
}

TEST(terminal, ctrl_c_drops_the_line_sets_the_status_and_raises_19)
{
	Screen s;
	s.terminal.type("abc\x03");
	CHECK_EQ(s.reg(TerminalDevice::StatusRegister) & TerminalDevice::StatusInterrupt, TerminalDevice::StatusInterrupt);
	CHECK(s.vm.interrupts().peek() == TerminalDevice::Interrupt);
	CHECK_EQ(s.terminal.availableBytes(), usize{ 0 });
	s.terminal.write(TerminalDevice::InterruptAckRegister, 1);
	CHECK_EQ(s.reg(TerminalDevice::StatusRegister) & TerminalDevice::StatusInterrupt, 0u);
}

TEST(terminal, ctrl_d_on_an_empty_line_is_the_end_of_input)
{
	Screen s;
	s.terminal.type("x\x04");
	CHECK_EQ(s.readInput(), std::string("x"));
	CHECK_EQ(s.reg(TerminalDevice::StatusRegister) & TerminalDevice::StatusEndOfInput, 0u);
	s.terminal.type("\x04");
	CHECK_EQ(s.reg(TerminalDevice::StatusRegister) & TerminalDevice::StatusEndOfInput, TerminalDevice::StatusEndOfInput);
	s.terminal.type("y\n");   // more typing: the end is over
	CHECK_EQ(s.reg(TerminalDevice::StatusRegister) & TerminalDevice::StatusEndOfInput, 0u);
}

TEST(terminal, closing_the_input_hands_over_a_line_without_its_enter)
{
	Screen s;
	s.terminal.type("last");
	s.terminal.closeInput();
	CHECK_EQ(s.readInput(), std::string("last"));
	CHECK_EQ(s.reg(TerminalDevice::StatusRegister) & TerminalDevice::StatusEndOfInput, TerminalDevice::StatusEndOfInput);
}

// --- The fault screen (plan/v2 F5.6) ---------------------------------------------------------------

TEST(terminal, the_fault_screen_is_painted_over_the_text_in_white_on_red)
{
	Screen s(40, 4);
	s.write("working...");
	s.terminal.showFault("Unhandled MemoryFault at 0x00000410");
	CHECK_EQ(s.text(), std::string("working...\nUnhandled MemoryFault at 0x00000410\n\n\n"));
	CHECK_EQ(s.cell(0, 1) >> 8, 0x1Fu);   // bright white (15) on red (1)
	CHECK_EQ(s.out, std::string("working..."));   // the report is the host's, not the program's output
}

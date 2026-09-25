// The text framebuffer (FramebufferDevice): the device on its own, and a program reaching it through its registers.
#include "device_test_machine.h"

TEST(text_framebuffer, the_framebuffer_shows_the_grid_it_was_given)
{
	Machine m{
		LoadBase(FramebufferBase), LoadBaseLow(FramebufferBase),
		Instruction::LI(1, 4),
		Instruction::STR(Base, 1, Off(FramebufferDevice::WidthRegister)),
		Instruction::LI(1, 2),
		Instruction::STR(Base, 1, Off(FramebufferDevice::HeightRegister)),
		Instruction::LI(1, FramebufferDevice::CommandClear),
		Instruction::STR(Base, 1, Off(FramebufferDevice::CommandRegister)),
		Instruction::LI(2, static_cast<u16>(SourceBuffer)),
		Instruction::LI(3, 8),
		Instruction::STR(Base, 2, Off(FramebufferDevice::BlockAddressRegister)),
		Instruction::STR(Base, 3, Off(FramebufferDevice::BlockLengthRegister)),
		Instruction::LI(4, FramebufferDevice::BlockCommandWrite),
		Instruction::STR(Base, 4, Off(FramebufferDevice::BlockCommandRegister)),
		Instruction::LI(1, FramebufferDevice::CommandPresent),
		Instruction::STR(Base, 1, Off(FramebufferDevice::CommandRegister)),
	};

	FramebufferDevice framebuffer{};
	framebuffer.attachTo(m.vm().io());

	std::string shown;
	framebuffer.setPresentSink([&shown](std::string_view frame) { shown = frame; });

	fill(m.memory(), SourceBuffer, "ab..#..#");
	m.step(18);

	// Rows, not a stream: the second four cells are the second line.
	CHECK_EQ(shown, std::string{ "ab..\n#..#\n" });
}

TEST(text_framebuffer, a_cell_that_would_move_the_terminals_own_cursor_is_shown_as_a_space)
{
	Machine m{ Instruction::NOP() };

	FramebufferDevice framebuffer{};
	framebuffer.attachTo(m.vm().io());

	framebuffer.write(FramebufferDevice::WidthRegister, 3);
	framebuffer.write(FramebufferDevice::HeightRegister, 1);
	framebuffer.write(FramebufferDevice::DataRegister, 'x');
	framebuffer.write(FramebufferDevice::DataRegister, 0x07); // a bell
	framebuffer.write(FramebufferDevice::DataRegister, 'y');

	CHECK_EQ(framebuffer.toText(), std::string{ "x y\n" });
}

TEST(text_framebuffer, a_latin1_cell_is_shown_as_its_code_point_in_utf8)
{
	Machine m{ Instruction::NOP() };

	FramebufferDevice framebuffer{};
	framebuffer.attachTo(m.vm().io());

	framebuffer.write(FramebufferDevice::WidthRegister, 4);
	framebuffer.write(FramebufferDevice::HeightRegister, 1);
	framebuffer.write(FramebufferDevice::DataRegister, 0xF1);   // n tilde
	framebuffer.write(FramebufferDevice::DataRegister, 'o');
	framebuffer.write(FramebufferDevice::DataRegister, 0x85);   // a C1 control: a space
	framebuffer.write(FramebufferDevice::DataRegister, 0xFF);   // y diaeresis

	CHECK_EQ(framebuffer.toText(), std::string{ "\xC3\xB1o \xC3\xBF\n" });
}

TEST(text_framebuffer, a_grid_larger_than_any_terminal_is_a_typo_and_is_ignored)
{
	Machine m{ Instruction::NOP() };

	FramebufferDevice framebuffer{};
	framebuffer.attachTo(m.vm().io());

	const u32 before = framebuffer.width();
	framebuffer.write(FramebufferDevice::WidthRegister, 100000);
	CHECK_EQ(framebuffer.width(), before);

	framebuffer.write(FramebufferDevice::HeightRegister, 0);
	CHECK_EQ(framebuffer.height(), u32{ 20 });
}

// --- Colour in the text grid -------------------------------------------------------------------

TEST(text_framebuffer, a_grid_without_colour_is_shown_as_plain_text)
{
	FramebufferDevice framebuffer{};
	framebuffer.write(FramebufferDevice::WidthRegister, 3);
	framebuffer.write(FramebufferDevice::HeightRegister, 1);
	framebuffer.write(FramebufferDevice::CommandRegister, FramebufferDevice::CommandClear);
	for (char c : std::string_view{ "abc" })
		framebuffer.write(FramebufferDevice::DataRegister, static_cast<u8>(c));

	std::string shown;
	framebuffer.setPresentSink([&](std::string_view frame) { shown = frame; });
	framebuffer.write(FramebufferDevice::CommandRegister, FramebufferDevice::CommandPresent);

	CHECK_EQ(shown, std::string{ "abc\n" });
	CHECK(!framebuffer.hasAttributes());
}

TEST(text_framebuffer, a_cells_attribute_sits_above_its_character_in_the_data_word)
{
	FramebufferDevice framebuffer{};
	framebuffer.write(FramebufferDevice::WidthRegister, 3);
	framebuffer.write(FramebufferDevice::HeightRegister, 1);
	framebuffer.write(FramebufferDevice::CommandRegister, FramebufferDevice::CommandClear);

	// Blue on red, blue on red, then the terminal's own colours.
	framebuffer.write(FramebufferDevice::DataRegister, (0x14u << FramebufferDevice::AttributeShift) | 'a');
	framebuffer.write(FramebufferDevice::DataRegister, (0x14u << FramebufferDevice::AttributeShift) | 'b');
	framebuffer.write(FramebufferDevice::DataRegister, 'c');

	std::string shown;
	framebuffer.setPresentSink([&](std::string_view frame) { shown = frame; });
	framebuffer.write(FramebufferDevice::CommandRegister, FramebufferDevice::CommandPresent);

	CHECK_EQ(shown, std::string{ "\x1b[34;41mab\x1b[0mc\n" });
	CHECK(framebuffer.hasAttributes());
}

TEST(text_framebuffer, a_row_never_leaves_the_terminal_painted_and_bright_colours_use_the_high_codes)
{
	FramebufferDevice framebuffer{};
	framebuffer.write(FramebufferDevice::WidthRegister, 2);
	framebuffer.write(FramebufferDevice::HeightRegister, 2);
	framebuffer.write(FramebufferDevice::CommandRegister, FramebufferDevice::CommandClear);

	framebuffer.write(FramebufferDevice::DataRegister, (0xF9u << FramebufferDevice::AttributeShift) | 'x'); // bright red on bright white
	framebuffer.write(FramebufferDevice::DataRegister, (0x02u << FramebufferDevice::AttributeShift) | 'y'); // green on black
	framebuffer.write(FramebufferDevice::DataRegister, 'z');
	framebuffer.write(FramebufferDevice::DataRegister, 'w');

	CHECK_EQ(framebuffer.toAnsiText(), std::string{ "\x1b[91;107mx\x1b[32;40my\x1b[0m\nzw\n" });
}

TEST(text_framebuffer, attributes_can_be_blitted_in_one_trigger_and_clearing_removes_them)
{
	Machine m{
		LoadBase(default_mmio::Framebuffer), LoadBaseLow(default_mmio::Framebuffer),
		Instruction::LI(1, 2),
		Instruction::STR(Base, 1, Off(FramebufferDevice::WidthRegister)),
		Instruction::LI(1, 1),
		Instruction::STR(Base, 1, Off(FramebufferDevice::HeightRegister)),
		Instruction::LI(1, FramebufferDevice::CommandClear),
		Instruction::STR(Base, 1, Off(FramebufferDevice::CommandRegister)),
		Instruction::LI(2, static_cast<u16>(SourceBuffer)),
		Instruction::LI(3, 2),
		Instruction::STR(Base, 2, Off(FramebufferDevice::BlockAddressRegister)),
		Instruction::STR(Base, 3, Off(FramebufferDevice::BlockLengthRegister)),
		Instruction::LI(4, FramebufferDevice::BlockCommandWrite),
		Instruction::STR(Base, 4, Off(FramebufferDevice::BlockCommandRegister)),
		Instruction::LI(2, static_cast<u16>(SourceBuffer + 16)),
		Instruction::STR(Base, 2, Off(FramebufferDevice::BlockAddressRegister)),
		Instruction::LI(4, FramebufferDevice::BlockCommandWriteAttributes),
		Instruction::STR(Base, 4, Off(FramebufferDevice::BlockCommandRegister)),
	};

	FramebufferDevice framebuffer{};
	framebuffer.attachTo(m.vm().io());

	fill(m.memory(), SourceBuffer, "hi");
	m.memory().writeUnchecked<u8>(Address(SourceBuffer + 16), 0x21);
	m.memory().writeUnchecked<u8>(Address(SourceBuffer + 17), 0x00);
	m.step(18);

	CHECK_EQ(framebuffer.toAnsiText(), std::string{ "\x1b[31;42mh\x1b[0mi\n" });
	CHECK_EQ(framebuffer.attributes()[0], u8{ 0x21 });

	framebuffer.write(FramebufferDevice::CommandRegister, FramebufferDevice::CommandClear);
	CHECK(!framebuffer.hasAttributes());
}

// The text framebuffer in a window: where a presented frame goes (the window, or the terminal), and the pixels
// a window is given for a frame. Everything here runs without a window: the device hands the host a frame,
// and TextRenderer turns it into pixels, so what would be shown can be checked to the pixel.

#include "framework.h"
#include <ceres/devices/video/display.h>
#include <ceres/devices/storage_devices.h>
#include <ceres/devices/video/text_renderer.h>
#include <string>
#include <string_view>
#include <vector>

using namespace ceres;
using namespace ceres::devices;
using namespace ceres::testing;

namespace
{
	FramebufferDevice::Frame makeFrame(u32 width, u32 height, std::string_view text = {}, u8 attribute = 0)
	{
		FramebufferDevice::Frame frame;
		frame.width = width;
		frame.height = height;
		frame.cells.assign(static_cast<usize>(width) * height, ' ');
		frame.attributes.assign(frame.cells.size(), attribute);
		for (usize i = 0; i < text.size() && i < frame.cells.size(); ++i)
			frame.cells[i] = static_cast<u8>(text[i]);
		return frame;
	}

	// One cell of a rendered frame as rows of '#' (lit) and '.' (background), for reading in a test.
	std::string art(const std::vector<u32>& pixels, u32 imageWidth, u32 cellColumn, u32 cellRow, u32 background)
	{
		std::string out;
		for (u32 y = 0; y < TextRenderer::CellHeight; ++y)
		{
			for (u32 x = 0; x < TextRenderer::CellWidth; ++x)
			{
				const u32 pixel = pixels[static_cast<usize>(cellRow * TextRenderer::CellHeight + y) * imageWidth + cellColumn * TextRenderer::CellWidth + x];
				out += pixel == background ? '.' : '#';
			}
			out += '\n';
		}
		return out;
	}

	// A device with a 3 x 1 grid holding "abc", ready to be presented.
	void fillAbc(FramebufferDevice& framebuffer)
	{
		framebuffer.writeWord(FramebufferDevice::WidthRegister, 3);
		framebuffer.writeWord(FramebufferDevice::HeightRegister, 1);
		framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandClear);
		for (char c : std::string_view{ "abc" })
			framebuffer.writeWord(FramebufferDevice::DataRegister, static_cast<u8>(c));
	}
}

// --- Where a frame goes ------------------------------------------------------------------------

TEST(text_window, a_frame_goes_to_the_terminal_when_the_host_has_no_window)
{
	FramebufferDevice framebuffer{};
	fillAbc(framebuffer);
	std::string shown;
	framebuffer.setPresentSink([&](std::string_view frame) { shown = frame; });

	CHECK_EQ(framebuffer.readUnsignedWord(FramebufferDevice::OutputRegister), FramebufferDevice::OutputTerminal);
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandPresent);
	CHECK_EQ(shown, std::string{ "abc\n" });

	FramebufferDevice::Frame frame;
	CHECK(!framebuffer.takeWindowFrame(frame));   // and nothing is waiting for a window
}

TEST(text_window, with_a_window_the_default_is_the_window_and_the_terminal_hears_nothing)
{
	FramebufferDevice framebuffer{};
	framebuffer.setWindowHost(true);
	fillAbc(framebuffer);
	std::string shown;
	framebuffer.setPresentSink([&](std::string_view frame) { shown = frame; });

	CHECK_EQ(framebuffer.readUnsignedWord(FramebufferDevice::ModeRegister), FramebufferDevice::ModeAuto);
	CHECK_EQ(framebuffer.readUnsignedWord(FramebufferDevice::OutputRegister), FramebufferDevice::OutputWindow);
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandPresent);
	CHECK(shown.empty());

	FramebufferDevice::Frame frame;
	CHECK(framebuffer.takeWindowFrame(frame));
	CHECK_EQ(frame.width, 3u);
	CHECK_EQ(frame.height, 1u);
	CHECK_EQ(std::string(frame.cells.begin(), frame.cells.end()), std::string{ "abc" });
	CHECK(!framebuffer.takeWindowFrame(frame));   // taken once
}

TEST(text_window, the_frame_is_the_grid_as_it_was_when_presented_not_as_it_is_when_the_window_draws_it)
{
	FramebufferDevice framebuffer{};
	framebuffer.setWindowHost(true);
	fillAbc(framebuffer);
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandPresent);

	// The program starts its next frame before the host has drawn this one.
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandClear);
	framebuffer.writeWord(FramebufferDevice::DataRegister, 'z');

	FramebufferDevice::Frame frame;
	CHECK(framebuffer.takeWindowFrame(frame));
	CHECK_EQ(std::string(frame.cells.begin(), frame.cells.end()), std::string{ "abc" });
}

TEST(text_window, presenting_twice_between_looks_leaves_the_later_frame)
{
	FramebufferDevice framebuffer{};
	framebuffer.setWindowHost(true);
	fillAbc(framebuffer);
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandPresent);
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandClear);
	framebuffer.writeWord(FramebufferDevice::DataRegister, 'z');
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandPresent);

	FramebufferDevice::Frame frame;
	CHECK(framebuffer.takeWindowFrame(frame));
	CHECK_EQ(static_cast<char>(frame.cells[0]), 'z');
}

TEST(text_window, a_program_can_ask_for_the_terminal_even_with_a_window)
{
	FramebufferDevice framebuffer{};
	framebuffer.setWindowHost(true);
	fillAbc(framebuffer);
	std::string shown;
	framebuffer.setPresentSink([&](std::string_view frame) { shown = frame; });

	framebuffer.writeWord(FramebufferDevice::ModeRegister, FramebufferDevice::ModeTerminal);
	CHECK_EQ(framebuffer.readUnsignedWord(FramebufferDevice::OutputRegister), FramebufferDevice::OutputTerminal);
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandPresent);
	CHECK_EQ(shown, std::string{ "abc\n" });
	FramebufferDevice::Frame frame;
	CHECK(!framebuffer.takeWindowFrame(frame));

	// and back again, at run time
	framebuffer.writeWord(FramebufferDevice::ModeRegister, FramebufferDevice::ModeWindow);
	CHECK_EQ(framebuffer.readUnsignedWord(FramebufferDevice::OutputRegister), FramebufferDevice::OutputWindow);
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandPresent);
	CHECK(framebuffer.takeWindowFrame(frame));
}

TEST(text_window, asking_for_a_window_where_there_is_none_still_shows_the_frame_on_the_terminal)
{
	FramebufferDevice framebuffer{};
	fillAbc(framebuffer);
	std::string shown;
	framebuffer.setPresentSink([&](std::string_view frame) { shown = frame; });

	framebuffer.writeWord(FramebufferDevice::ModeRegister, FramebufferDevice::ModeWindow);
	CHECK_EQ(framebuffer.readUnsignedWord(FramebufferDevice::ModeRegister), FramebufferDevice::ModeWindow);   // what was asked for
	CHECK_EQ(framebuffer.readUnsignedWord(FramebufferDevice::OutputRegister), FramebufferDevice::OutputTerminal); // what happens
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandPresent);
	CHECK_EQ(shown, std::string{ "abc\n" });
}

TEST(text_window, only_the_three_modes_are_accepted)
{
	FramebufferDevice framebuffer{};
	framebuffer.writeWord(FramebufferDevice::ModeRegister, FramebufferDevice::ModeTerminal);
	framebuffer.writeWord(FramebufferDevice::ModeRegister, 7);
	CHECK_EQ(framebuffer.readUnsignedWord(FramebufferDevice::ModeRegister), FramebufferDevice::ModeTerminal);   // ignored
	CHECK_EQ(framebuffer.mode(), FramebufferDevice::ModeTerminal);
}

TEST(text_window, a_host_that_cannot_open_its_window_gives_the_frame_and_the_rest_to_the_terminal)
{
	FramebufferDevice framebuffer{};
	framebuffer.setWindowHost(true);
	fillAbc(framebuffer);
	std::string shown;
	int frames = 0;
	framebuffer.setPresentSink([&](std::string_view frame) { shown = frame; ++frames; });

	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandPresent);
	FramebufferDevice::Frame frame;
	CHECK(framebuffer.takeWindowFrame(frame));
	framebuffer.fallBackToTerminal();               // the host could not draw it
	CHECK_EQ(shown, std::string{ "abc\n" });
	CHECK_EQ(frames, 1);

	fillAbc(framebuffer);
	framebuffer.writeWord(FramebufferDevice::CommandRegister, FramebufferDevice::CommandPresent);
	CHECK_EQ(frames, 2);                             // later frames go the same way
	CHECK(!framebuffer.takeWindowFrame(frame));
	CHECK_EQ(framebuffer.readUnsignedWord(FramebufferDevice::OutputRegister), FramebufferDevice::OutputTerminal);
}

TEST(text_window, the_display_counts_the_frames_it_presents)
{
	DisplayDevice display{};
	CHECK_EQ(display.presentCount(), u64{ 0 });
	display.writeWord(DisplayDevice::CommandRegister, DisplayDevice::CommandPresent);
	display.writeWord(DisplayDevice::CommandRegister, DisplayDevice::CommandPresent);
	CHECK_EQ(display.presentCount(), u64{ 2 });
}

// --- The pixels ---------------------------------------------------------------------------------

TEST(text_window, a_frame_is_eight_by_sixteen_pixels_a_cell)
{
	const auto frame = makeFrame(40, 20);
	CHECK_EQ(TextRenderer::imageWidth(frame), 320u);
	CHECK_EQ(TextRenderer::imageHeight(frame), 320u);
	std::vector<u32> pixels;
	TextRenderer::render(frame, pixels);
	CHECK_EQ(pixels.size(), usize{ 320 } * 320);
}

TEST(text_window, an_empty_cell_is_the_default_background_and_the_default_colours_are_light_grey_on_black)
{
	std::vector<u32> pixels;
	TextRenderer::render(makeFrame(2, 1, "A"), pixels);
	CHECK_EQ(pixels[15 * 16 + 15], 0x000000u);         // the second cell, a space
	bool sawLit = false;
	for (u32 p : pixels)
		if (p == 0xAAAAAAu) sawLit = true;
	CHECK(sawLit);
	for (u32 p : pixels)
		CHECK(p == 0x000000u || p == 0xAAAAAAu);       // and nothing else
}

TEST(text_window, a_letter_is_the_five_by_seven_shape_drawn_one_wide_and_two_tall_with_a_margin)
{
	std::vector<u32> pixels;
	TextRenderer::render(makeFrame(1, 1, "H"), pixels);
	CHECK_EQ(art(pixels, 8, 0, 0, 0x000000),
		std::string{
			"........\n"
			".#...#..\n" ".#...#..\n"
			".#...#..\n" ".#...#..\n"
			".#...#..\n" ".#...#..\n"
			".#####..\n" ".#####..\n"
			".#...#..\n" ".#...#..\n"
			".#...#..\n" ".#...#..\n"
			".#...#..\n" ".#...#..\n"
			"........\n" });
}

TEST(text_window, a_dot_of_the_font_is_where_the_font_table_says)
{
	// '!' is a column with a gap: rows 0-4, then blank, then the dot (0x04 is the middle of five).
	std::vector<u32> pixels;
	TextRenderer::render(makeFrame(1, 1, "!"), pixels);
	for (u32 y = 1; y <= 10; ++y)
		CHECK(TextRenderer::glyphPixel('!', 3, y));
	for (u32 y = 11; y <= 12; ++y)
		CHECK(!TextRenderer::glyphPixel('!', 3, y));
	CHECK(TextRenderer::glyphPixel('!', 3, 13));
	CHECK(TextRenderer::glyphPixel('!', 3, 14));
	CHECK(!TextRenderer::glyphPixel('!', 2, 5));
	CHECK(!TextRenderer::glyphPixel('!', 4, 5));
	CHECK_EQ(pixels[5 * 8 + 3], 0xAAAAAAu);
}

TEST(text_window, no_printable_character_draws_outside_its_cell_or_is_blank)
{
	for (u32 c = 0x20; c < 0x7F; ++c)
	{
		bool any = false;
		for (u32 y = 0; y < TextRenderer::CellHeight; ++y)
			for (u32 x = 0; x < TextRenderer::CellWidth; ++x)
				any = any || TextRenderer::glyphPixel(static_cast<u8>(c), x, y);
		CHECK_EQ(any, c != ' ');
	}
	CHECK(!TextRenderer::glyphPixel('A', 8, 1));    // outside the cell
	CHECK(!TextRenderer::glyphPixel('A', 1, 16));
}

TEST(text_window, every_latin1_character_has_a_glyph_but_the_no_break_space)
{
	for (u32 c = 0xA0; c <= 0xFF; ++c)
	{
		bool any = false;
		for (u32 y = 0; y < TextRenderer::CellHeight; ++y)
			for (u32 x = 0; x < TextRenderer::CellWidth; ++x)
				any = any || TextRenderer::glyphPixel(static_cast<u8>(c), x, y);
		CHECK_EQ(any, c != 0xA0);
	}
	// An accented letter is its letter with the accent on top: the body of an e acute is the body of an e.
	for (u32 y = 5; y <= 14; ++y)
		for (u32 x = 0; x < TextRenderer::CellWidth; ++x)
			CHECK_EQ(TextRenderer::glyphPixel(0xE9, x, y), TextRenderer::glyphPixel('e', x, y));
	CHECK(TextRenderer::glyphPixel(0xE9, 4, 1));   // the acute's top dot, right of the middle
	CHECK(!TextRenderer::glyphPixel('e', 4, 1));
}

TEST(text_window, a_control_byte_is_a_space)
{
	for (u32 c : { 0u, 7u, 10u, 31u, 127u, 128u, 150u, 159u })
		for (u32 y = 0; y < TextRenderer::CellHeight; ++y)
			for (u32 x = 0; x < TextRenderer::CellWidth; ++x)
				CHECK(!TextRenderer::glyphPixel(static_cast<u8>(c), x, y));
}

TEST(text_window, the_strokes_of_a_box_join_up_across_cells)
{
	// A frame drawn with + - | must not show as dashes: a vertical bar reaches the top and bottom of its cell, a
	// horizontal one both sides, so neighbours touch.
	for (u32 y = 0; y < 16; ++y)
		CHECK(TextRenderer::glyphPixel('|', 3, y) && TextRenderer::glyphPixel('|', 4, y));
	for (u32 x = 0; x < 8; ++x)
		CHECK(TextRenderer::glyphPixel('-', x, 7) && TextRenderer::glyphPixel('-', x, 8));

	std::vector<u32> pixels;
	TextRenderer::render(makeFrame(1, 2, "||"), pixels);
	CHECK_EQ(pixels[15 * 8 + 3], 0xAAAAAAu);        // the last row of the upper cell
	CHECK_EQ(pixels[16 * 8 + 3], 0xAAAAAAu);        // the first row of the lower one
}

TEST(text_window, a_plus_reaches_only_toward_the_strokes_it_can_join)
{
	// A box corner: a stroke to its right and one below, so it has just those two arms.
	auto frame = makeFrame(3, 2, "+-");
	frame.cells[3] = '|';
	std::vector<u32> pixels;
	TextRenderer::render(frame, pixels);
	std::string expected;
	for (int i = 0; i < 7; ++i) expected += "........\n";
	for (int i = 0; i < 2; ++i) expected += "...#####\n";
	for (int i = 0; i < 7; ++i) expected += "...##...\n";
	CHECK_EQ(art(pixels, 24, 0, 0, 0x000000), expected);
}

TEST(text_window, a_crossing_reaches_out_on_all_four_sides_and_a_lone_plus_is_the_fonts_plus)
{
	CHECK(TextRenderer::plusPixel(15, 0, 7) && TextRenderer::plusPixel(15, 7, 8));
	CHECK(TextRenderer::plusPixel(15, 3, 0) && TextRenderer::plusPixel(15, 4, 15));
	CHECK(!TextRenderer::plusPixel(15, 0, 0));
	for (u32 y = 0; y < 16; ++y)
		for (u32 x = 0; x < 8; ++x)
			CHECK_EQ(TextRenderer::plusPixel(0, x, y), TextRenderer::glyphPixelFont('+', x, y));
}

TEST(text_window, an_attribute_gives_a_cell_its_foreground_and_background)
{
	// FB_ATTR(red, blue): low nibble 1, high nibble 4.
	std::vector<u32> pixels;
	TextRenderer::render(makeFrame(1, 1, "H", 0x41), pixels);
	CHECK_EQ(pixels[0], 0x0000AAu);                  // background: blue
	bool sawRed = false;
	for (u32 p : pixels)
	{
		CHECK(p == 0x0000AAu || p == 0xAA0000u);
		sawRed = sawRed || p == 0xAA0000u;
	}
	CHECK(sawRed);
}

TEST(text_window, the_bright_colours_are_the_upper_eight_and_may_be_a_background_too)
{
	std::vector<u32> pixels;
	TextRenderer::render(makeFrame(1, 1, "H", 0xB9), pixels);   // bright red on bright yellow
	CHECK_EQ(pixels[0], 0xFFFF55u);
	bool sawRed = false;
	for (u32 p : pixels)
		sawRed = sawRed || p == 0xFF5555u;
	CHECK(sawRed);
	CHECK_EQ(TextRenderer::colour(0), 0x000000u);
	CHECK_EQ(TextRenderer::colour(7), 0xAAAAAAu);
	CHECK_EQ(TextRenderer::colour(15), 0xFFFFFFu);
}

TEST(text_window, each_cell_has_its_own_colours)
{
	auto frame = makeFrame(2, 1, "HH");
	frame.attributes[1] = 0x2F;                      // white on green
	std::vector<u32> pixels;
	TextRenderer::render(frame, pixels);
	CHECK_EQ(pixels[0], 0x000000u);                  // first cell: default background
	CHECK_EQ(pixels[8], 0x00AA00u);                  // second cell: green
}

TEST(text_window, a_frame_that_is_shorter_than_it_claims_is_all_background_not_a_crash)
{
	FramebufferDevice::Frame frame;
	frame.width = 4;
	frame.height = 4;
	frame.cells.assign(3, 'x');
	frame.attributes.assign(3, 0);
	std::vector<u32> pixels;
	TextRenderer::render(frame, pixels);
	CHECK_EQ(pixels.size(), usize{ 32 } * 64);
	for (u32 p : pixels)
		CHECK_EQ(p, 0x000000u);
}

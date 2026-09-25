// The display (DisplayDevice): the device on its own, and a program reaching it through its registers.
#include "device_test_machine.h"

// --- The pixel display -------------------------------------------------------------------------

TEST(display, the_display_shows_the_pixels_it_was_given)
{
	CeresVM vm{};
	DisplayDevice display{};
	display.attachTo(vm.io());

	display.write(DisplayDevice::WidthRegister, 2);
	display.write(DisplayDevice::HeightRegister, 1);

	// Two pixels in RAM, 0x00RRGGBB: red then green.
	vm.memory().writeUnchecked<u32>(Address(SourceBuffer), 0x00FF0000u);
	vm.memory().writeUnchecked<u32>(Address(SourceBuffer + 4), 0x0000FF00u);

	display.write(DisplayDevice::BlockAddressRegister, SourceBuffer);
	display.write(DisplayDevice::BlockLengthRegister, 8); // two pixels
	display.write(DisplayDevice::BlockCommandRegister, DisplayDevice::BlockCommandWrite);

	u32 shownWidth = 0, shownHeight = 0;
	std::vector<u32> shown;
	display.setFrameSink([&](u32 width, u32 height, std::span<const u32> pixels)
	{
		shownWidth = width;
		shownHeight = height;
		shown.assign(pixels.begin(), pixels.end());
	});

	display.write(DisplayDevice::CommandRegister, DisplayDevice::CommandPresent);

	CHECK_EQ(shownWidth, u32{ 2 });
	CHECK_EQ(shownHeight, u32{ 1 });
	CHECK_EQ(shown.size(), usize{ 2 });
	if (shown.size() == 2)
	{
		CHECK_EQ(shown[0], u32{ 0x00FF0000u });
		CHECK_EQ(shown[1], u32{ 0x0000FF00u });
	}
}

TEST(display, a_display_pixel_can_be_written_one_at_a_time)
{
	DisplayDevice display{};

	display.write(DisplayDevice::WidthRegister, 3);
	display.write(DisplayDevice::HeightRegister, 1);

	display.write(DisplayDevice::DataRegister, 0x00112233u);
	display.write(DisplayDevice::DataRegister, 0x00445566u);

	CHECK_EQ(display.pixels()[0], u32{ 0x00112233u });
	CHECK_EQ(display.pixels()[1], u32{ 0x00445566u });
	CHECK_EQ(display.pixels()[2], u32{ 0 }); // The third cell was never written.
}

TEST(display, a_display_surface_larger_than_any_screen_is_a_typo_and_is_ignored)
{
	DisplayDevice display{};

	const u32 before = display.width();
	display.write(DisplayDevice::WidthRegister, 100000);
	CHECK_EQ(display.width(), before);

	display.write(DisplayDevice::HeightRegister, 0);
	CHECK_EQ(display.height(), u32{ 200 });
}

TEST(display, a_display_clear_fills_black)
{
	DisplayDevice display{};

	display.write(DisplayDevice::WidthRegister, 2);
	display.write(DisplayDevice::HeightRegister, 1);
	display.write(DisplayDevice::DataRegister, 0x00FFFFFFu); // white

	display.write(DisplayDevice::CommandRegister, DisplayDevice::CommandClear);

	CHECK_EQ(display.pixels()[0], u32{ 0 });
}

TEST(display, an_indexed_display_goes_through_its_palette_and_scrolls)
{
	CeresVM vm{ Memory::DefaultSize };
	DisplayDevice display{};
	display.attachTo(vm.io());
	display.write(DisplayDevice::WidthRegister, 3);
	display.write(DisplayDevice::HeightRegister, 2);
	display.write(DisplayDevice::ModeRegister, DisplayDevice::ModeIndexed);
	CHECK_EQ(display.read(DisplayDevice::ModeRegister), DisplayDevice::ModeIndexed);
	display.write(DisplayDevice::PaletteIndexRegister, 1);
	display.write(DisplayDevice::PaletteDataRegister, 0x00FF0000u);   // 1: red
	display.write(DisplayDevice::PaletteDataRegister, 0x0000FF00u);   // 2: green
	const u8 indices[6] = { 1, 2, 0, 0, 0, 2 };
	for (u32 i = 0; i < 6; ++i)
		vm.memory().writeUnchecked<u8>(Address(0x2000 + i), indices[i]);
	display.write(DisplayDevice::BlockAddressRegister, 0x2000);
	display.write(DisplayDevice::BlockLengthRegister, 6);            // a byte a pixel
	display.write(DisplayDevice::BlockCommandRegister, DisplayDevice::BlockCommandWrite);
	std::vector<u32> shown;
	display.setFrameSink([&](u32, u32, std::span<const u32> pixels) { shown.assign(pixels.begin(), pixels.end()); });
	display.write(DisplayDevice::CommandRegister, DisplayDevice::CommandPresent);
	CHECK(shown.size() == 6 && shown[0] == 0x00FF0000u && shown[1] == 0x0000FF00u && shown[2] == 0u && shown[5] == 0x0000FF00u);
	display.write(DisplayDevice::ScrollXRegister, 1);                // the second column shows at the left
	display.write(DisplayDevice::ScrollYRegister, 1);                // and the second row at the top
	display.write(DisplayDevice::CommandRegister, DisplayDevice::CommandPresent);
	CHECK(shown.size() == 6 && shown[0] == 0u && shown[1] == 0x0000FF00u && shown[2] == 0u);   // row 1, from column 1, wrapping
	CHECK(shown.size() == 6 && shown[3] == 0x0000FF00u && shown[4] == 0u && shown[5] == 0x00FF0000u);
	CHECK(display.frame()[3] == 0x0000FF00u);                             // what a window draws
}

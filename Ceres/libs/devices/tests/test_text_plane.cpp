#include "device_test_machine.h"

#include <ceres/devices/video/default_font.h>
#include <ceres/devices/video/gpu.h>

namespace
{
	u64 fnv1a(const std::vector<u32>& pixels)
	{
		u64 hash = 1469598103934665603ull;
		for (u32 pixel : pixels)
			for (u32 b = 0; b < 4; ++b)
			{
				hash ^= (pixel >> (8 * b)) & 0xFF;
				hash *= 1099511628211ull;
			}
		return hash;
	}

	u32 reg(CeresVM& vm, u32 offset) { return vm.io().read(default_mmio::Gpu + Address(offset)); }

	void putCell(CeresVM& vm, u32 address, u16 cell)
	{
		vm.vram().write<u16>(address - Vram::BaseValue, cell);
	}
}

TEST(text_plane, the_boot_layout_is_published_in_the_registers)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	using video::TextPlane;

	// standard: the largest terminal is 160x45 cells of 2 bytes, then the font, the palette and the scrollback, each at
	// a multiple of 256 (plan/v2 SPEC 7.4).
	CHECK_EQ(reg(vm, TextPlane::CellsBaseRegister), 0xA0000000u);
	CHECK_EQ(reg(vm, TextPlane::FontBaseRegister), 0xA0000000u + 14592u);   // 160 * 45 * 2 = 14400, rounded up
	CHECK_EQ(reg(vm, TextPlane::PaletteBaseRegister), 0xA0000000u + 14592u + 4096u);
	CHECK_EQ(reg(vm, TextPlane::ScrollbackBaseRegister), 0xA0000000u + 14592u + 4096u + 1024u);
	CHECK_EQ(reg(vm, TextPlane::ScrollbackLinesRegister), 256u * 1024u / (80u * 2u));
	CHECK_EQ(reg(vm, TextPlane::ColsRegister), 80u);
	CHECK_EQ(reg(vm, TextPlane::RowsRegister), 30u);
	CHECK_EQ(reg(vm, TextPlane::GlyphCountRegister), 256u);

	// The font and the palette are in VRAM, where the registers say.
	const u32 font = reg(vm, TextPlane::FontBaseRegister) - Vram::BaseValue;
	for (u32 i = 0; i < FontGlyphCount * FontGlyphBytes; ++i)
		if (vm.vram().read<u8>(font + i) != DefaultFont8x16[i]) { CHECK(false); break; }
	const u32 palette = reg(vm, TextPlane::PaletteBaseRegister) - Vram::BaseValue;
	CHECK_EQ(vm.vram().read<u32>(palette + 4 * 1), 0xAA0000u);    // ANSI red
	CHECK_EQ(vm.vram().read<u32>(palette + 4 * 15), 0xFFFFFFu);
	CHECK_EQ(vm.vram().read<u32>(palette + 4 * 196), 0xFF0000u);  // the cube's red
	CHECK_EQ(vm.vram().read<u32>(palette + 4 * 232), 0x080808u);  // the first grey

	// A change of resolution changes the grid.
	vm.io().write(default_mmio::Gpu + GpuDevice::WidthRegister, 1280);
	vm.io().write(default_mmio::Gpu + GpuDevice::HeightRegister, 720);
	CHECK_EQ(reg(vm, TextPlane::ColsRegister), 160u);
	CHECK_EQ(reg(vm, TextPlane::RowsRegister), 45u);
	gpu.detachFrom(vm.io());
}

TEST(text_plane, cells_compose_into_the_frame)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	using video::TextPlane;
	vm.io().write(default_mmio::Gpu + GpuDevice::WidthRegister, 64);   // 8 x 2 cells
	vm.io().write(default_mmio::Gpu + GpuDevice::HeightRegister, 32);
	vm.io().write(default_mmio::Gpu + GpuDevice::BackgroundColorRegister, 0x000080);

	const u32 cells = reg(vm, TextPlane::CellsBaseRegister);
	putCell(vm, cells + 0, static_cast<u16>('H' | (15 << 8) | (1 << 12)));   // white on red
	putCell(vm, cells + 2, static_cast<u16>('i' | (10 << 8)));               // bright green on the transparent background
	putCell(vm, cells + 16, static_cast<u16>(0x96 | (14 << 8)));              // a full block, second row

	video::VideoFrame frame;
	gpu.compose(frame);
	CHECK_EQ(frame.width, 64u);
	CHECK_EQ(frame.height, 32u);
	// The H's cell: its left stroke is lit, the margin column is the red background.
	CHECK_EQ(frame.pixels[1 * 64 + 1], 0xFFFFFFu);
	CHECK_EQ(frame.pixels[1 * 64 + 0], 0xAA0000u);
	// The i's background is 0, so the background colour shows through it.
	CHECK_EQ(frame.pixels[0 * 64 + 8], 0x000080u);
	// The block fills its cell with its ink.
	CHECK_EQ(frame.pixels[16 * 64 + 0], 0x55FFFFu);
	CHECK_EQ(frame.pixels[31 * 64 + 7], 0x55FFFFu);
	// A blank cell is a space over the background colour.
	CHECK_EQ(frame.pixels[20 * 64 + 40], 0x000080u);
	CHECK_EQ(fnv1a(frame.pixels), 7224454607525615795ull);   // the whole picture, fixed: any change to the scanout shows here

	// A block cursor on the i swaps its ink and background.
	vm.io().write(default_mmio::Gpu + Address(TextPlane::CursorXRegister), 1);
	vm.io().write(default_mmio::Gpu + Address(TextPlane::CursorShapeRegister), TextPlane::CursorBlock);
	gpu.compose(frame);
	CHECK_EQ(frame.pixels[0 * 64 + 8], 0x55FF55u);

	// Turned off, the plane shows nothing.
	vm.io().write(default_mmio::Gpu + Address(TextPlane::EnableRegister), 0);
	gpu.compose(frame);
	CHECK_EQ(frame.pixels[1 * 64 + 1], 0x000080u);
	gpu.detachFrom(vm.io());
}

TEST(text_plane, the_scrollback_shows_above_the_screen)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	using video::TextPlane;
	TextPlane& text = gpu.textPlane();

	// Two rows went into the ring: 'A' then 'B'. Scrolled back by one, the top row is the newest, 'B'.
	vm.vram().write<u16>(text.scrollbackRowAddress(0) - Vram::BaseValue, static_cast<u16>('A' | (7 << 8)));
	vm.vram().write<u16>(text.scrollbackRowAddress(1) - Vram::BaseValue, static_cast<u16>('B' | (7 << 8)));
	putCell(vm, text.cellAddress(0, 0), static_cast<u16>('C' | (7 << 8)));
	text.setScrollback(2, 2);
	text.setScrollY(1);
	CHECK_EQ(text.shownCell(vm.vram(), 0, 0) & 0xFF, u32{ 'B' });
	CHECK_EQ(text.shownCell(vm.vram(), 0, 1) & 0xFF, u32{ 'C' });
	text.setScrollY(5);   // no further back than the ring holds
	CHECK_EQ(text.scrollY(), 2u);
	CHECK_EQ(text.shownCell(vm.vram(), 0, 0) & 0xFF, u32{ 'A' });
	gpu.detachFrom(vm.io());
}

TEST(text_plane, a_present_applies_a_new_cells_base_at_the_vertical_blank)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	using video::TextPlane;
	const u32 moved = 0xA0100000;
	vm.io().write(default_mmio::Gpu + Address(TextPlane::CellsBaseRegister), moved);
	CHECK_EQ(reg(vm, TextPlane::CellsBaseRegister), moved);          // reads what was written
	CHECK_EQ(gpu.textPlane().cellsBase(), 0xA0000000u);              // the scanout keeps the old one
	vm.io().write(default_mmio::Gpu + GpuDevice::PresentRegister, 1);
	Scheduler& events = vm.io().scheduler();
	events.service(events.cycleOf(gpu, GpuDevice::VblankEvent));
	CHECK_EQ(gpu.textPlane().cellsBase(), moved);
	gpu.detachFrom(vm.io());
}

TEST(text_plane, the_screen_reads_back_as_text)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	vm.io().write(default_mmio::Gpu + GpuDevice::WidthRegister, 32);
	vm.io().write(default_mmio::Gpu + GpuDevice::HeightRegister, 32);
	const video::TextPlane& text = gpu.textPlane();
	putCell(vm, text.cellAddress(0, 0), 'o');
	putCell(vm, text.cellAddress(1, 0), 'k');
	putCell(vm, text.cellAddress(0, 1), 0x82);          // the box corner ┌
	putCell(vm, text.cellAddress(1, 1), 0xE9);          // é
	CHECK_EQ(gpu.screenText(), std::string("ok\n┌é\n"));
	CHECK_EQ(video::glyphFor(0x2518), 0x85u);
	CHECK_EQ(video::glyphFor(0x4E2D), u32{ '?' });
	gpu.detachFrom(vm.io());
}

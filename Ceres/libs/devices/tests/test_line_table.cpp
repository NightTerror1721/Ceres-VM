#include "device_test_machine.h"

#include <ceres/devices/video/gpu.h>

// The line table and the scan line by line (plan/v2 SPEC 7.5 and 7.6): registers the table writes at the start of a
// line, or the CPU in a line's interrupt, change the picture from that line or the next one, and stay written.

namespace
{
	using video::LayerControl;
	using video::OamEntry;
	using video::Retro2D;

	void set(CeresVM& vm, u32 offset, u32 value) { vm.io().write(default_mmio::Gpu + Address(offset), value); }
	u32 get(CeresVM& vm, u32 offset) { return vm.io().read(default_mmio::Gpu + Address(offset)); }

	constexpr u32 TableAt = 0xA0100000;
	constexpr u32 PaletteAt = 0xA0110000;
	constexpr u32 MapAt = 0xA0120000;
	constexpr u32 TilesAt = 0xA0130000;
	constexpr u32 ShiftsAt = 0xA0140000;
	constexpr u32 OamAt = 0xA0150000;
	constexpr u32 PicturesAt = 0xA0160000;
	constexpr u32 ScreenWidth = 64;
	constexpr u32 ScreenHeight = 32;
	constexpr u32 Start = 0x000080;

	// A machine that halts, over and over, so each step runs to the next event of the GPU.
	Machine halting() { return Machine{ Instruction::STI(), Instruction::HALT(), Instruction::HALT(), Instruction::HALT(), Instruction::HALT(),
		Instruction::HALT(), Instruction::HALT(), Instruction::HALT(), Instruction::HALT(), Instruction::HALT() }; }

	void screen(CeresVM& vm)
	{
		set(vm, GpuDevice::WidthRegister.value(), ScreenWidth);
		set(vm, GpuDevice::HeightRegister.value(), ScreenHeight);
		set(vm, GpuDevice::ModeRegister.value(), 2);
		set(vm, GpuDevice::BackgroundColorRegister.value(), Start);
		set(vm, video::TextPlane::EnableRegister, 0);
	}

	u32 entry(CeresVM& vm, u32 index, u32 line, u32 target, u32 value)
	{
		vm.vram().write<u32>(TableAt - Vram::BaseValue + 8 * index, line | (target << 16));
		vm.vram().write<u32>(TableAt - Vram::BaseValue + 8 * index + 4, value);
		return index + 1;
	}

	void table(CeresVM& vm, u32 count)
	{
		set(vm, Retro2D::LineTableBaseRegister, TableAt);
		set(vm, Retro2D::LineTableCountRegister, count);
	}

	// Steps until the GPU has counted `frames` vertical blanks.
	void runTo(Machine& m, GpuDevice& gpu, u64 frames)
	{
		for (u32 guard = 0; gpu.frameCounter() < frames && guard < 100000; ++guard)
			m.step();
	}

	u32 at(const video::VideoFrame& frame, u32 x, u32 y) { return frame.pixels[y * frame.width + x]; }
}

TEST(line_table, every_register_of_v2_is_declared)
{
	// An undeclared offset is dropped by the bus (plan/v2 D19): every register of 0x300-0x3FF that V2 has must be there.
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	const u32 offsets[] = {
		Retro2D::TilePaletteBaseRegister, Retro2D::SpritePaletteBaseRegister, Retro2D::OamBaseRegister, Retro2D::SpriteTileBaseRegister,
		Retro2D::SpriteControlRegister, Retro2D::SpriteStatusRegister, Retro2D::SpriteLimitRegister, Retro2D::LineTableBaseRegister,
		Retro2D::LineTableCountRegister, Retro2D::AffineControlRegister, Retro2D::AffineMapBaseRegister, Retro2D::AffineTileBaseRegister,
		Retro2D::AffineOriginXRegister, Retro2D::AffineOriginYRegister, Retro2D::AffineMatrixABRegister, Retro2D::AffineMatrixCDRegister,
	};
	for (u32 offset : offsets)
		CHECK(vm.io().declares(default_mmio::Gpu + Address(offset)));
	for (u32 layer = 0; layer < Retro2D::LayerCount; ++layer)
		for (u32 field = 0; field <= Retro2D::LayerLineScrollBaseRegister; field += 4)
			CHECK(vm.io().declares(default_mmio::Gpu + Address(Retro2D::layerRegister(layer, field))));
	CHECK(!vm.io().declares(default_mmio::Gpu + Address(0x324)));   // V3's
	gpu.detachFrom(vm.io());
}

TEST(line_table, entries_change_the_background_line_by_line_every_frame)
{
	Machine m = halting();
	CeresVM& vm = m.vm();
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	screen(vm);
	u32 n = 0;
	for (u32 y = 0; y < ScreenHeight; y += 2)
		n = entry(vm, n, y, GpuDevice::BackgroundColorRegister.value(), 0x010203u * y);
	table(vm, n);

	runTo(m, gpu, 1);
	video::VideoFrame frame;
	gpu.composeScanned(frame);
	for (u32 y = 0; y < ScreenHeight; ++y)
		CHECK_EQ(at(frame, 5, y), 0x010203u * (y & ~1u));
	// What the table wrote stays written: the last entry's colour, until line 0 of the next frame writes again.
	CHECK_EQ(get(vm, GpuDevice::BackgroundColorRegister.value()), 0x010203u * 30);
	runTo(m, gpu, 2);
	gpu.composeScanned(frame);
	CHECK_EQ(at(frame, 5, 0), 0u);
	CHECK_EQ(at(frame, 5, 31), 0x010203u * 30);
	// compose() is the screen with the registers as they are, on every line.
	gpu.compose(frame);
	CHECK_EQ(at(frame, 5, 0), 0x010203u * 30);
	gpu.detachFrom(vm.io());
}

TEST(line_table, entries_out_of_order_or_for_other_registers_are_skipped)
{
	Machine m = halting();
	CeresVM& vm = m.vm();
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	screen(vm);
	const u32 background = GpuDevice::BackgroundColorRegister.value();
	u32 n = 0;
	n = entry(vm, n, 4, background, 0x111111);
	n = entry(vm, n, 3, background, 0x222222);                        // its line has passed: skipped
	n = entry(vm, n, 6, Retro2D::LineTableCountRegister, 0);          // not the table's own registers
	n = entry(vm, n, 6, GpuDevice::PresentRegister.value(), 1);       // nor the rest of the GPU
	n = entry(vm, n, 6, Retro2D::SpriteStatusRegister, 0xFFFF);       // nor a register that is only read
	n = entry(vm, n, 7, background + 1, 0x333333);                    // nor a misaligned one
	n = entry(vm, n, 8, video::BitmapPlane::ScrollXRegister, 5);      // the bitmap's scroll is fine
	n = entry(vm, n, 9, background, 0x444444);
	n = entry(vm, n, 1000, background, 0x555555);                     // past the screen: never reached
	table(vm, n);

	runTo(m, gpu, 1);
	video::VideoFrame frame;
	gpu.composeScanned(frame);
	CHECK_EQ(at(frame, 0, 3), Start);
	CHECK_EQ(at(frame, 0, 4), 0x111111u);
	CHECK_EQ(at(frame, 0, 8), 0x111111u);
	CHECK_EQ(at(frame, 0, 9), 0x444444u);
	CHECK_EQ(at(frame, 0, 31), 0x444444u);
	CHECK_EQ(get(vm, Retro2D::LineTableCountRegister), n);
	CHECK_EQ(get(vm, GpuDevice::StatusRegister.value()) & GpuDevice::StatusFlipPending, 0u);
	CHECK_EQ(get(vm, video::BitmapPlane::ScrollXRegister), 5u);
	// The count is kept to what a table may hold.
	set(vm, Retro2D::LineTableCountRegister, 0xFFFFFFFFu);
	CHECK_EQ(get(vm, Retro2D::LineTableCountRegister), Retro2D::MaxLineTableEntries);
	gpu.detachFrom(vm.io());
}

TEST(line_table, a_write_in_a_lines_interrupt_shows_from_the_next_line)
{
	Machine m = halting();
	CeresVM& vm = m.vm();
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	screen(vm);
	// The table turns the background grey at line 5; the handler of line 10's interrupt reads it back into r9 and makes
	// the background red.
	entry(vm, 0, 5, GpuDevice::BackgroundColorRegister.value(), 0x808080);
	table(vm, 1);
	m.installHandler(GpuDevice::LineInterrupt, Address(0x800), {
		Instruction::LUI(13, 0xFF40),
		Instruction::LDR(9, 13, static_cast<i16>(GpuDevice::BackgroundColorRegister.value())),
		Instruction::LUI(1, 0x00FF),
		Instruction::STR(13, 1, static_cast<i16>(GpuDevice::BackgroundColorRegister.value())),
		Instruction::IRET(),
	});
	set(vm, GpuDevice::LineCompareRegister.value(), 10);
	set(vm, GpuDevice::IrqEnableRegister.value(), GpuDevice::IrqLine);

	runTo(m, gpu, 1);
	CHECK_EQ(m.reg(9), 0x808080u);
	video::VideoFrame frame;
	gpu.composeScanned(frame);
	CHECK_EQ(at(frame, 0, 4), Start);
	CHECK_EQ(at(frame, 0, 5), 0x808080u);
	CHECK_EQ(at(frame, 0, 10), 0x808080u);
	CHECK_EQ(at(frame, 0, 11), 0xFF0000u);
	CHECK_EQ(at(frame, 0, 31), 0xFF0000u);
	gpu.detachFrom(vm.io());
}

TEST(line_table, a_scroll_a_line_from_the_table_matches_the_layers_own_scroll_table)
{
	// The same wavy picture two ways: the layer's table of a scroll a line, and the line table writing ScrollX.
	Machine m = halting();
	CeresVM& vm = m.vm();
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	screen(vm);
	Vram& vram = vm.vram();
	u32 seed = 7;
	const auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
	for (u32 i = 0; i < 32 * 32; ++i)
		vram.write<u16>(MapAt - Vram::BaseValue + 2 * i, static_cast<u16>(next() % 16));
	for (u32 i = 0; i < 16 * 32; ++i)
		vram.write<u8>(TilesAt - Vram::BaseValue + i, static_cast<u8>(next()));
	for (u32 i = 0; i < 256; ++i)
		vram.write<u32>(PaletteAt - Vram::BaseValue + 4 * i, next() & 0xFFFFFF);
	u32 n = 0;
	for (u32 y = 0; y < ScreenHeight; ++y)
	{
		vram.write<u32>(ShiftsAt - Vram::BaseValue + 4 * y, (y * 3) & 0xFFFF);
		n = entry(vm, n, y, Retro2D::layerRegister(0, Retro2D::LayerScrollXRegister), 100 + y * 3);
	}
	set(vm, Retro2D::TilePaletteBaseRegister, PaletteAt);
	set(vm, Retro2D::layerRegister(0, Retro2D::LayerMapBaseRegister), MapAt);
	set(vm, Retro2D::layerRegister(0, Retro2D::LayerTileBaseRegister), TilesAt);
	set(vm, Retro2D::layerRegister(0, Retro2D::LayerLineScrollBaseRegister), ShiftsAt);
	set(vm, Retro2D::layerRegister(0, Retro2D::LayerScrollXRegister), 100);
	set(vm, Retro2D::layerRegister(0, Retro2D::LayerControlRegister), LayerControl::Enable | LayerControl::LineScroll);
	video::VideoFrame byLayer;
	gpu.compose(byLayer);

	set(vm, Retro2D::layerRegister(0, Retro2D::LayerControlRegister), LayerControl::Enable);
	table(vm, n);
	runTo(m, gpu, 1);
	video::VideoFrame byTable;
	gpu.composeScanned(byTable);
	CHECK(byTable.pixels == byLayer.pixels);
	gpu.detachFrom(vm.io());
}

TEST(line_table, the_sprite_status_counts_each_stretch_with_its_own_registers)
{
	Machine m = halting();
	CeresVM& vm = m.vm();
	GpuDevice gpu;
	gpu.configure(GpuDevice::Config{ .gpuClockHz = 2'000'000, .maxLevel = 2, .maxWidth = 256, .maxHeight = 192, .refresh = 60, .spritesPerLine = 4 });
	gpu.attachTo(vm.io());
	screen(vm);
	// Six sprites 8 high on lines 20-27.
	for (u32 i = 0; i < 6; ++i)
	{
		vm.vram().write<u32>(OamAt - Vram::BaseValue + 16 * i, (i * 8) | (20u << 16));
		vm.vram().write<u32>(OamAt - Vram::BaseValue + 16 * i + 4, OamEntry::Visible);
	}
	set(vm, Retro2D::OamBaseRegister, OamAt);
	set(vm, Retro2D::SpriteTileBaseRegister, PicturesAt);
	set(vm, Retro2D::SpriteControlRegister, Retro2D::SpritesOn);
	// Off for lines 20-23, on again from 24: the first line over the limit is 24.
	entry(vm, 0, 20, Retro2D::SpriteControlRegister, 0);
	entry(vm, 1, 24, Retro2D::SpriteControlRegister, Retro2D::SpritesOn);
	table(vm, 2);
	runTo(m, gpu, 1);
	CHECK_EQ(get(vm, Retro2D::SpriteStatusRegister), Retro2D::SpriteOverflow | (24u << Retro2D::SpriteOverflowLineShift));
	// Off from 20 to the end: no line is over.
	entry(vm, 1, 24, Retro2D::SpriteControlRegister, 0);
	runTo(m, gpu, 2);
	CHECK_EQ(get(vm, Retro2D::SpriteStatusRegister), 0u);
	gpu.detachFrom(vm.io());
}

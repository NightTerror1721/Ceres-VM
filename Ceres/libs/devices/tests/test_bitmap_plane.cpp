#include "device_test_machine.h"

#include <ceres/devices/video/gpu.h>

namespace
{
	using video::BitmapPlane;
	using video::CopyEngine;

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

	void set(CeresVM& vm, u32 offset, u32 value) { vm.io().write(default_mmio::Gpu + Address(offset), value); }
	u32 get(CeresVM& vm, u32 offset) { return vm.io().read(default_mmio::Gpu + Address(offset)); }

	constexpr u32 Picture = 0xA0200000;
	constexpr u32 Palette = 0xA0300000;

	// A 16x16 screen in V1, the text plane off, showing a 4x2 picture in `format` at Picture (it repeats across).
	void showPicture(CeresVM& vm, u32 format, u32 pitch, std::initializer_list<u8> bytes)
	{
		set(vm, GpuDevice::WidthRegister.value(), 16);
		set(vm, GpuDevice::HeightRegister.value(), 16);
		set(vm, GpuDevice::ModeRegister.value(), 1);
		set(vm, video::TextPlane::EnableRegister, 0);
		set(vm, BitmapPlane::EnableRegister, 1);
		set(vm, BitmapPlane::FormatRegister, format);
		set(vm, BitmapPlane::WidthRegister, 4);
		set(vm, BitmapPlane::HeightRegister, 2);
		set(vm, BitmapPlane::PitchRegister, pitch);
		set(vm, BitmapPlane::BaseRegister, Picture);
		set(vm, BitmapPlane::PaletteBaseRegister, Palette);
		u32 at = Picture - Vram::BaseValue;
		for (u8 byte : bytes)
			vm.vram().write<u8>(at++, byte);
		for (u32 i = 0; i < 256; ++i)
			vm.vram().write<u32>(Palette - Vram::BaseValue + 4 * i, 0x010101u * i);
		// The bases take effect at the vertical blank after a Present.
		set(vm, GpuDevice::PresentRegister.value(), 1);
		Scheduler& events = vm.io().scheduler();
		GpuDevice* gpu = static_cast<GpuDevice*>(vm.io().deviceAt(0x40));
		events.service(events.cycleOf(*gpu, GpuDevice::VblankEvent));
	}
}

TEST(bitmap_plane, every_format_decodes_to_the_right_colours)
{
	struct Case
	{
		u32 format;
		u32 pitch;
		std::initializer_list<u8> bytes;
		u32 first, second;   // the colours of pixels (0,0) and (1,0)
	};
	const Case cases[] = {
		{ 0, 1, { 0b01000000, 0b10000000 }, 0x000000, 0x010101 },                           // I1: 0, 1
		{ 1, 1, { 0b11100000, 0 }, 0x030303, 0x020202 },                                      // I2: 3, 2
		{ 2, 2, { 0xA5, 0x00, 0, 0 }, 0x0A0A0A, 0x050505 },                                   // I4: 10, 5
		{ 3, 4, { 200, 7, 0, 0, 0, 0, 0, 0 }, 0xC8C8C8, 0x070707 },                           // I8
		{ 4, 8, { 0x00, 0xF8, 0xE0, 0x07, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 0xFF0000, 0x00FF00 },   // RGB565
		{ 5, 8, { 0x1F, 0x80, 0x1F, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 0x0000FF, 0x000080 },   // ARGB1555: opaque blue, then transparent over the background
		{ 6, 16, { 0x56, 0x34, 0x12, 0x00, 0xFF, 0xFF, 0xFF, 0x77 }, 0x123456, 0xFFFFFF },   // XRGB8888 ignores the top byte
		{ 7, 16, { 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x80 }, 0xFF0000, 0x8080C0 },   // ARGB8888: opaque red, then white at half alpha over 0x000080
	};
	u64 combined = 0;
	for (const Case& c : cases)
	{
		CeresVM vm;
		GpuDevice gpu;
		gpu.attachTo(vm.io());
		set(vm, GpuDevice::BackgroundColorRegister.value(), 0x000080);
		showPicture(vm, c.format, c.pitch, c.bytes);
		video::VideoFrame frame;
		gpu.compose(frame);
		CHECK_EQ(frame.pixels[0], c.first);
		CHECK_EQ(frame.pixels[1], c.second);   // ARGB8888: (255 * 128 + 127) / 255 = 128 on red and green, 192 on blue
		// The picture is 4 wide: column 4 shows column 0 again, and row 2 row 0.
		CHECK_EQ(frame.pixels[4], frame.pixels[0]);
		CHECK_EQ(frame.pixels[2 * 16], frame.pixels[0]);
		combined = combined * 31 + fnv1a(frame.pixels);
		gpu.detachFrom(vm.io());
	}
	CHECK_EQ(combined, 3835619276825596864ull);   // every format's whole picture, fixed
}

TEST(bitmap_plane, scrolling_moves_the_picture_under_the_screen)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	showPicture(vm, 3, 4, { 1, 2, 3, 4, 5, 6, 7, 8 });
	set(vm, BitmapPlane::ScrollXRegister, 1);
	set(vm, BitmapPlane::ScrollYRegister, 1);
	video::VideoFrame frame;
	gpu.compose(frame);
	CHECK_EQ(frame.pixels[0], 0x060606u);   // row 1, column 1
	CHECK_EQ(frame.pixels[3], 0x050505u);   // wraps to column 0
	gpu.detachFrom(vm.io());
}

TEST(bitmap_plane, in_mode_0_the_bitmap_is_not_shown)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	showPicture(vm, 3, 4, { 9, 9, 9, 9, 9, 9, 9, 9 });
	set(vm, GpuDevice::ModeRegister.value(), 0);
	video::VideoFrame frame;
	gpu.compose(frame);
	CHECK_EQ(frame.pixels[0], 0u);
	gpu.detachFrom(vm.io());
}

TEST(bitmap_plane, two_and_three_buffers_flip_at_the_vertical_blank)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	Scheduler& events = vm.io().scheduler();
	const auto blank = [&] { events.service(events.cycleOf(gpu, GpuDevice::VblankEvent)); };
	const u32 a = get(vm, BitmapPlane::BaseRegister);
	const u32 b = get(vm, BitmapPlane::BackBaseRegister);
	const u32 c = get(vm, BitmapPlane::SpareBaseRegister);
	CHECK(a != 0 && b > a && c > b);   // the boot layout has room for three pictures of the screen

	set(vm, BitmapPlane::BuffersRegister, 2);
	set(vm, GpuDevice::PresentRegister.value(), 1);
	CHECK_EQ(get(vm, BitmapPlane::BackBaseRegister), b);   // nothing moves before the blank
	blank();
	CHECK_EQ(gpu.bitmapPlane().shownBase(), b);
	CHECK_EQ(get(vm, BitmapPlane::BackBaseRegister), a);

	// Three: the back buffer is queued at once and the spare becomes the one to draw into.
	set(vm, BitmapPlane::BuffersRegister, 3);
	set(vm, BitmapPlane::BaseRegister, a);
	set(vm, BitmapPlane::BackBaseRegister, b);
	set(vm, BitmapPlane::SpareBaseRegister, c);
	set(vm, GpuDevice::PresentRegister.value(), 1);
	CHECK_EQ(get(vm, BitmapPlane::BackBaseRegister), c);
	CHECK_EQ(gpu.bitmapPlane().shownBase(), b);            // still the old picture until the blank
	blank();
	CHECK_EQ(gpu.bitmapPlane().shownBase(), b);            // b was queued, and is shown ...
	CHECK_EQ(get(vm, BitmapPlane::BaseRegister), b);
	CHECK_EQ(get(vm, BitmapPlane::SpareBaseRegister), a);  // ... and the one it replaced is the spare
	set(vm, GpuDevice::PresentRegister.value(), 1);
	CHECK_EQ(get(vm, BitmapPlane::BackBaseRegister), a);
	blank();
	CHECK_EQ(gpu.bitmapPlane().shownBase(), c);
	CHECK_EQ(get(vm, BitmapPlane::SpareBaseRegister), b);
	gpu.detachFrom(vm.io());
}

TEST(copy_engine, a_copy_lands_on_the_cycle_its_gpu_time_ends)
{
	Machine m{ Instruction::HALT() };
	CeresVM& vm = m.vm();
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	fill(vm.memory(), SourceBuffer, "copied by the gpu");
	set(vm, GpuDevice::IrqEnableRegister.value(), GpuDevice::IrqCopy);
	set(vm, CopyEngine::SrcRegister, SourceBuffer);
	set(vm, CopyEngine::DstRegister, 0xA0400000);
	set(vm, CopyEngine::LengthRegister, 17);
	set(vm, CopyEngine::CommandRegister, CopyEngine::CommandCopy);
	CHECK_EQ(get(vm, CopyEngine::StatusRegister), CopyEngine::StatusBusy);
	CHECK_EQ(get(vm, GpuDevice::StatusRegister.value()) & GpuDevice::StatusBusy, GpuDevice::StatusBusy);

	// 16 + ceil(17 / 4) = 21 GPU cycles at 200 MHz: 5.25 CPU cycles at 50 MHz, so it is done on cycle 6.
	CHECK_EQ(vm.io().scheduler().cycleOf(gpu, GpuDevice::CopyEvent), 6u);
	CHECK(vm.vram().read<u8>(0x400000) == 0);
	m.step();   // the HALT
	m.step();   // the halted machine jumps to the copy's end
	CHECK_EQ(vm.engine().cycles(), 6u);
	CHECK_EQ(get(vm, CopyEngine::StatusRegister), 0u);
	std::string copied;
	for (u32 i = 0; i < 17; ++i)
		copied.push_back(static_cast<char>(vm.vram().read<u8>(0x400000 + i)));
	CHECK_EQ(copied, std::string("copied by the gpu"));
	CHECK(vm.interrupts().peek() == GpuDevice::CopyInterrupt);
	gpu.detachFrom(vm.io());
}

TEST(copy_engine, a_fill_repeats_its_pattern_and_a_bad_address_faults)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	set(vm, CopyEngine::DstRegister, 0xA0400001);
	set(vm, CopyEngine::LengthRegister, 6);
	set(vm, CopyEngine::FillValueRegister, 0x44332211);
	set(vm, CopyEngine::CommandRegister, CopyEngine::CommandFill);
	Scheduler& events = vm.io().scheduler();
	events.service(events.cycleOf(gpu, GpuDevice::CopyEvent));
	// Aligned to the address, as a word store would put it: byte 1 of the word is 0x22.
	CHECK_EQ(vm.vram().read<u8>(0x400000), u8{ 0 });
	CHECK_EQ(vm.vram().read<u8>(0x400001), u8{ 0x22 });
	CHECK_EQ(vm.vram().read<u8>(0x400004), u8{ 0x11 });
	CHECK_EQ(vm.vram().read<u8>(0x400006), u8{ 0x33 });
	CHECK_EQ(vm.vram().read<u8>(0x400007), u8{ 0 });

	set(vm, GpuDevice::IrqEnableRegister.value(), GpuDevice::IrqFault);
	set(vm, CopyEngine::SrcRegister, 0x90000000);   // the empty region
	set(vm, CopyEngine::CommandRegister, CopyEngine::CommandCopy);
	CHECK_EQ(get(vm, CopyEngine::StatusRegister), CopyEngine::StatusError);
	CHECK_EQ(get(vm, GpuDevice::FaultCodeRegister.value()), GpuDevice::FaultBadAddress);
	CHECK_EQ(get(vm, GpuDevice::FaultAddressRegister.value()), 0x90000000u);
	CHECK(vm.interrupts().peek() == GpuDevice::FaultInterrupt);
	CHECK_EQ(events.cycleOf(gpu, GpuDevice::CopyEvent), NoScheduledEvent);
	gpu.detachFrom(vm.io());
}

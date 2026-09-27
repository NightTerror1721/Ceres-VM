#include "device_test_machine.h"

#include <ceres/devices/video/gpu.h>

namespace
{
	// 50 MHz at 60 Hz: 833 333 cycles a frame and 20 over, which the frames share (plan/v2 SPEC 3.1).
	constexpr u64 Hz = 50'000'000;

	u64 frameStart(u64 frame) { return frame * Hz / 60; }

	// Where frame n's vertical blank starts at 640x480: line 480 of its 525.
	u64 expectedVblank(u64 frame)
	{
		return frameStart(frame) + 480 * (frameStart(frame + 1) - frameStart(frame)) / 525;
	}
}

TEST(gpu, identifies_itself_and_boots_in_text_mode_at_640x480)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());

	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::IdRegister), GpuDevice::IdValue);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::VramSizeRegister), static_cast<u32>(vm.vram().size()));
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::GpuClockHzRegister), 200'000'000u);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::ModeRegister), 0u);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::WidthRegister), 640u);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::HeightRegister), 480u);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::RefreshRegister), 60u);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::LinesTotalRegister), 525u);
	// V0 and V1 built, every bitmap format, no hardware executor.
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::CapsRegister), 0xFF03u);
	gpu.detachFrom(vm.io());
}

TEST(gpu, mode_never_goes_above_what_is_built_or_what_the_profile_allows)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	vm.io().write(default_mmio::Gpu + GpuDevice::ModeRegister, 5);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::ModeRegister), GpuDevice::ImplementedLevel);
	gpu.detachFrom(vm.io());

	GpuDevice text;
	text.configure(GpuDevice::Config{ .gpuClockHz = 2'000'000, .maxLevel = 0, .maxWidth = 256, .maxHeight = 192, .refresh = 50 });
	text.attachTo(vm.io());
	vm.io().write(default_mmio::Gpu + GpuDevice::ModeRegister, 1);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::ModeRegister), 0u);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::MaxLevelRegister), 0u);
	// The boot resolution is the profile's largest when 640x480 does not fit.
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::WidthRegister), 256u);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::HeightRegister), 192u);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::RefreshRegister), 50u);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::LinesTotalRegister), 262u);   // 192 + 45 is below the minimum
	// A resolution above the profile's is clamped to it.
	vm.io().write(default_mmio::Gpu + GpuDevice::WidthRegister, 1920);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::WidthRegister), 256u);
	text.detachFrom(vm.io());
}

TEST(gpu, the_vertical_blank_comes_on_the_exact_cycle_with_the_remainder_carried)
{
	Machine m{ Instruction::HALT() };
	GpuDevice gpu;
	gpu.attachTo(m.vm().io());
	m.step();   // the HALT

	// A halted machine jumps from event to event: every step is one vertical blank. The interrupt is not enabled,
	// so nothing ends the halt.
	for (u64 frame = 0; frame < 120; ++frame)
	{
		m.step();
		CHECK_EQ(m.vm().engine().cycles(), expectedVblank(frame));
		CHECK_EQ(gpu.frameCounter(), frame + 1);
	}
	// Sixty frames are exactly one second of the CPU clock: the 20 cycles over were not lost.
	CHECK_EQ(gpu.display().frameStart(60), Hz);
	CHECK_EQ(gpu.display().frameStart(120), 2 * Hz);
	CHECK(!m.vm().interrupts().hasPending());
	gpu.detachFrom(m.vm().io());
}

TEST(gpu, line_compare_raises_its_interrupt_when_the_scan_reaches_the_line)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	Scheduler& events = vm.io().scheduler();

	vm.io().write(default_mmio::Gpu + GpuDevice::LineCompareRegister, 100);
	CHECK_EQ(events.cycleOf(gpu, GpuDevice::LineEvent), NoScheduledEvent);   // not enabled: no event
	vm.io().write(default_mmio::Gpu + GpuDevice::IrqEnableRegister, GpuDevice::IrqLine);

	const u64 line100 = frameStart(0) + 100 * (frameStart(1) - frameStart(0)) / 525;
	CHECK_EQ(events.cycleOf(gpu, GpuDevice::LineEvent), line100);
	events.service(line100);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::IrqStatusRegister) & GpuDevice::IrqLine, GpuDevice::IrqLine);
	CHECK(vm.interrupts().peek() == GpuDevice::LineInterrupt);
	// The next one is the same line of the next frame.
	CHECK_EQ(events.cycleOf(gpu, GpuDevice::LineEvent), frameStart(1) + 100 * (frameStart(2) - frameStart(1)) / 525);

	// Writing the bit back clears the status.
	vm.io().write(default_mmio::Gpu + GpuDevice::IrqStatusRegister, GpuDevice::IrqLine);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::IrqStatusRegister) & GpuDevice::IrqLine, 0u);
	gpu.detachFrom(vm.io());
}

TEST(gpu, the_display_controller_counts_lines_within_a_frame)
{
	video::DisplayController display;
	display.setClock(Hz);
	display.setHeight(480);
	CHECK_EQ(display.lineAt(0), 0u);
	CHECK_EQ(display.lineAt(expectedVblank(0) - 1), 479u);
	CHECK_EQ(display.lineAt(expectedVblank(0)), 480u);
	CHECK(display.inVblank(expectedVblank(0)));
	CHECK_EQ(display.lineAt(frameStart(1)), 0u);
	CHECK_EQ(display.frameAt(frameStart(1) - 1), 0u);
	CHECK_EQ(display.frameAt(frameStart(1)), 1u);
	CHECK_EQ(display.frameAt(50'000'000), 60u);
}

TEST(gpu, a_present_is_applied_at_the_next_vertical_blank)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	std::vector<bool> seen;
	gpu.setVblankObserver([&](bool presented) { seen.push_back(presented); });

	vm.io().write(default_mmio::Gpu + GpuDevice::PresentRegister, 1);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::StatusRegister) & GpuDevice::StatusFlipPending, GpuDevice::StatusFlipPending);
	Scheduler& events = vm.io().scheduler();
	events.service(events.cycleOf(gpu, GpuDevice::VblankEvent));
	events.service(events.cycleOf(gpu, GpuDevice::VblankEvent));
	CHECK_EQ(seen.size(), usize{ 2 });
	CHECK(seen[0]);
	CHECK(!seen[1]);
	CHECK_EQ(vm.io().read(default_mmio::Gpu + GpuDevice::StatusRegister) & GpuDevice::StatusFlipPending, 0u);
	gpu.detachFrom(vm.io());
}

TEST(gpu, composes_the_background_colour_and_black_with_the_display_off)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	vm.io().write(default_mmio::Gpu + GpuDevice::WidthRegister, 64);
	vm.io().write(default_mmio::Gpu + GpuDevice::HeightRegister, 32);
	vm.io().write(default_mmio::Gpu + GpuDevice::BackgroundColorRegister, 0x123456);
	video::VideoFrame frame;
	gpu.compose(frame);
	CHECK_EQ(frame.width, 64u);
	CHECK_EQ(frame.height, 32u);
	CHECK_EQ(frame.pixels.size(), usize{ 64 * 32 });
	CHECK_EQ(frame.pixels[0], 0x123456u);
	CHECK_EQ(frame.pixels.back(), 0x123456u);

	vm.io().write(default_mmio::Gpu + GpuDevice::ControlRegister, 0);
	gpu.compose(frame);
	CHECK_EQ(frame.pixels[0], 0u);
	gpu.detachFrom(vm.io());
}

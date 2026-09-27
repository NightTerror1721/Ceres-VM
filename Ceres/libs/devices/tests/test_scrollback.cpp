// The terminal's scrollback ring (plan/v2 SPEC 8.2).
#include "device_test_machine.h"

#include <ceres/devices/terminal/scrollback.h>
#include <ceres/devices/video/gpu.h>

TEST(scrollback, rows_go_into_the_ring_and_the_oldest_gives_way)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	video::TextPlane& plane = gpu.textPlane();
	plane.write(video::TextPlane::ScrollbackLinesRegister, 3);   // a small ring

	for (u16 row = 0; row < 5; ++row)
	{
		vm.vram().write<u16>(plane.cellAddress(0, 0) - Vram::BaseValue, static_cast<u16>('A' + row));
		term::Scrollback::push(plane, vm.vram(), plane.cellAddress(0, 0));
	}
	CHECK_EQ(plane.scrollbackCount(), 3u);
	CHECK_EQ(plane.scrollbackHead(), 2u);   // five into three: the next goes where C is

	// Scrolled back all the way: the oldest kept (C) at the top, then D and E, then the screen.
	term::Scrollback::scroll(plane, 10);
	CHECK_EQ(plane.scrollY(), 3u);
	CHECK_EQ(plane.shownCell(vm.vram(), 0, 0) & 0xFF, u32{ 'C' });
	CHECK_EQ(plane.shownCell(vm.vram(), 0, 1) & 0xFF, u32{ 'D' });
	CHECK_EQ(plane.shownCell(vm.vram(), 0, 2) & 0xFF, u32{ 'E' });
	term::Scrollback::scroll(plane, -1);
	CHECK_EQ(plane.shownCell(vm.vram(), 0, 0) & 0xFF, u32{ 'D' });
	term::Scrollback::scroll(plane, -10);
	CHECK_EQ(plane.scrollY(), 0u);

	term::Scrollback::clear(plane);
	CHECK_EQ(plane.scrollbackCount(), 0u);
	gpu.detachFrom(vm.io());
}

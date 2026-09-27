#include <ceres/devices/terminal/scrollback.h>

#include <algorithm>
#include <cstring>

namespace ceres::devices::term
{
	void Scrollback::push(video::TextPlane& plane, vm::Vram& vram, u32 rowAddress) noexcept
	{
		const u32 lines = plane.scrollbackLines();
		if (lines == 0)
			return;
		const u32 bytes = plane.cols() * plane.cellBytes();
		const u32 head = plane.scrollbackHead() % lines;
		const u32 to = plane.scrollbackRowAddress(head);
		if (!vram.backs(rowAddress, bytes) || !vram.backs(to, bytes))
			return;
		std::memmove(vram.span(to - vm::Vram::BaseValue, bytes), vram.data() + (rowAddress - vm::Vram::BaseValue), bytes);
		plane.setScrollback((head + 1) % lines, std::min(plane.scrollbackCount() + 1, lines));
	}

	void Scrollback::scroll(video::TextPlane& plane, i32 rows) noexcept
	{
		const i64 target = static_cast<i64>(plane.scrollY()) + rows;
		plane.setScrollY(static_cast<u32>(std::clamp<i64>(target, 0, plane.scrollbackCount())));
	}
}

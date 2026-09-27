#pragma once

// The rows that scrolled off the top of the terminal (plan/v2 SPEC 8.2): a ring in VRAM that the text plane shows
// above the screen when ScrollY is not 0 (text_plane.h). The terminal pushes a row into it as the row leaves the
// screen, and Shift+PageUp and Shift+PageDown in the window move the view through it. The program does not see it.

#include <ceres/devices/video/text_plane.h>

namespace ceres::devices::term
{
	class Scrollback
	{
	public:
		// Copies the screen row at `rowAddress` into the ring, the oldest row giving way when it is full.
		static void push(video::TextPlane& plane, vm::Vram& vram, u32 rowAddress) noexcept;
		// Moves the view `rows` further back (negative: forward), no further than the ring holds.
		static void scroll(video::TextPlane& plane, i32 rows) noexcept;
		// Empties the ring.
		static void clear(video::TextPlane& plane) noexcept { plane.setScrollback(0, 0); }
	};
}

#pragma once

// The display's timing (plan/v2 SPEC 3.1 and 7.3): frames at the refresh rate and lines within them, on the CPU's
// clock. Frame n starts n * CpuClockHz / Refresh cycles after the origin - rounded down, so the remainder of each
// division is carried into the next frame and nothing drifts (50 MHz at 60 Hz is 833 333 cycles a frame and 20 over,
// which the frames share). A frame has LinesTotal lines, the visible height plus 45 of blanking and never fewer than
// 262, spread over its cycles the same way. The vertical blank starts at the first line past the visible height.
//
// Pure arithmetic over the cycle count: the GPU asks it when its next event is due and what VCount reads.

#include <ceres/core/base/types.h>

namespace ceres::devices::video
{
	class DisplayController
	{
	public:
		static inline constexpr u32 BlankingLines = 45;
		static inline constexpr u32 MinLinesTotal = 262;

	private:
		u64 _cpuHz = 50'000'000;
		u32 _refresh = 60;
		u32 _height = 480;
		u64 _origin = 0;   // the cycle frame 0 starts on

	public:
		void setClock(u64 cpuHz) noexcept { _cpuHz = cpuHz == 0 ? 1 : cpuHz; }
		void setRefresh(u32 refresh) noexcept { _refresh = refresh == 50 ? 50 : 60; }
		void setHeight(u32 height) noexcept { _height = height; }
		// Frame 0 starts on `cycle`: the machine powered on, or restarted.
		void restart(u64 cycle) noexcept { _origin = cycle; }

		u64 cpuHz() const noexcept { return _cpuHz; }
		u32 refresh() const noexcept { return _refresh; }
		u32 height() const noexcept { return _height; }
		u64 origin() const noexcept { return _origin; }
		u32 linesTotal() const noexcept { return _height + BlankingLines < MinLinesTotal ? MinLinesTotal : _height + BlankingLines; }

		// The cycle frame `frame` starts on.
		u64 frameStart(u64 frame) const noexcept { return _origin + frame * _cpuHz / _refresh; }

		// The cycle line `line` of frame `frame` starts on.
		u64 lineStart(u64 frame, u32 line) const noexcept
		{
			const u64 start = frameStart(frame);
			const u64 length = frameStart(frame + 1) - start;
			return start + static_cast<u64>(line) * length / linesTotal();
		}

		// The cycle the vertical blank of frame `frame` starts on: its first line past the visible ones.
		u64 vblankStart(u64 frame) const noexcept { return lineStart(frame, _height); }

		// The frame `cycle` falls in (0 before the origin).
		u64 frameAt(u64 cycle) const noexcept
		{
			if (cycle <= _origin)
				return 0;
			const u64 t = cycle - _origin;
			u64 frame = t / _cpuHz * _refresh + (t % _cpuHz) * _refresh / _cpuHz;
			while (frame > 0 && frameStart(frame) > cycle)
				--frame;
			while (frameStart(frame + 1) <= cycle)
				++frame;
			return frame;
		}

		// The line `cycle` falls on in its frame: what VCount reads.
		u32 lineAt(u64 cycle) const noexcept
		{
			const u64 frame = frameAt(cycle);
			const u64 start = frameStart(frame);
			const u64 length = frameStart(frame + 1) - start;
			if (cycle < start || length == 0)
				return 0;
			const u32 total = linesTotal();
			u32 line = static_cast<u32>((cycle - start) * total / length);
			while (line > 0 && lineStart(frame, line) > cycle)
				--line;
			while (line + 1 < total && lineStart(frame, line + 1) <= cycle)
				++line;
			return line;
		}

		bool inVblank(u64 cycle) const noexcept { return lineAt(cycle) >= _height; }

		// The first cycle at or after `cycle` that line `line` starts on (in this frame or a later one).
		u64 nextLineStart(u64 cycle, u32 line) const noexcept
		{
			u64 frame = frameAt(cycle);
			u64 at = lineStart(frame, line);
			if (at < cycle)
				at = lineStart(frame + 1, line);
			return at;
		}
	};
}

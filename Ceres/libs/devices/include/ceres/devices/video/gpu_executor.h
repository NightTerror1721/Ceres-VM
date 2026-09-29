#pragma once

// What turns the GPU's state into a picture. The software executor (software_executor.h) is the reference, exact
// to the bit (plan/v2 SPEC 1.6); a hardware one on SDL_GPU comes in F12 and may only be faster.

#include <ceres/core/base/types.h>
#include <ceres/devices/video/bitmap_plane.h>
#include <ceres/devices/video/retro2d.h>
#include <ceres/vm/vram.h>

#include <span>
#include <vector>

namespace ceres::devices::video
{
	class TextPlane;

	// One composed picture: Width x Height pixels, row by row, 0x00RRGGBB.
	struct VideoFrame
	{
		u32 width = 0;
		u32 height = 0;
		std::vector<u32> pixels;
		u64 number = 0;   // the GPU's FrameCounter when it was composed
	};

	// What the scanout reads for a stretch of lines, from `firstLine` to the next stretch's (plan/v2 SPEC 7.6): the
	// registers of everything but the text plane as they were when the scan reached that line. The VRAM behind them
	// is read when the frame is composed.
	struct LineState
	{
		u32 firstLine = 0;
		u32 mode = 0;               // the video level: 0 text only, 1 adds the bitmap plane, 2 the tile layers and sprites
		bool displayOn = true;      // Control bit 0; off, the lines are black
		u32 background = 0;         // BackgroundColor, 0x00RRGGBB
		BitmapPlane bitmap;
		Retro2D retro;

		// The same registers, whatever line they start on.
		bool sameAs(const LineState& other) const noexcept
		{
			return mode == other.mode && displayOn == other.displayOn && background == other.background && bitmap == other.bitmap && retro == other.retro;
		}
	};

	// Everything the scanout reads for one frame (plan/v2 SPEC 7.2 and 7.6).
	struct ScanoutState
	{
		u32 width = 0;
		u32 height = 0;
		u64 frameCounter = 0;                // what a blinking cursor counts with
		const TextPlane* text = nullptr;     // as it is when the frame is composed
		std::span<const LineState> lines;    // in order, the first from line 0
	};

	class GpuExecutor
	{
	public:
		virtual ~GpuExecutor() = default;

		// Composes the screen into `frame` (resized to the state's width and height).
		virtual void compose(const ScanoutState& state, const vm::Vram& vram, VideoFrame& frame) = 0;
	};
}

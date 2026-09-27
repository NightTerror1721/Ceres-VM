#pragma once

// What turns the GPU's state into a picture. The software executor (software_executor.h) is the reference, exact
// to the bit (plan/v2 SPEC 1.6); a hardware one on SDL_GPU comes in F12 and may only be faster.

#include <ceres/core/base/types.h>
#include <ceres/vm/vram.h>

#include <vector>

namespace ceres::devices::video
{
	class TextPlane;
	class BitmapPlane;

	// One composed picture: Width x Height pixels, row by row, 0x00RRGGBB.
	struct VideoFrame
	{
		u32 width = 0;
		u32 height = 0;
		std::vector<u32> pixels;
		u64 number = 0;   // the GPU's FrameCounter when it was composed
	};

	// Everything the scanout reads, as the GPU holds it at the moment of composing (plan/v2 SPEC 7.2).
	struct ScanoutState
	{
		u32 width = 0;
		u32 height = 0;
		u32 mode = 0;               // the video level: 0 text only, 1 adds the bitmap plane
		bool displayOn = true;      // Control bit 0; off, the screen is black
		u32 background = 0;         // BackgroundColor, 0x00RRGGBB
		u64 frameCounter = 0;       // what a blinking cursor counts with
		const TextPlane* text = nullptr;
		const BitmapPlane* bitmap = nullptr;
	};

	class GpuExecutor
	{
	public:
		virtual ~GpuExecutor() = default;

		// Composes the screen into `frame` (resized to the state's width and height).
		virtual void compose(const ScanoutState& state, const vm::Vram& vram, VideoFrame& frame) = 0;
	};
}

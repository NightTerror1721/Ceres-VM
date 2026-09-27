#pragma once

// The pixel formats of the GPU's bitmap plane (plan/v2 SPEC 7.1, V1), in the order of their codes, and the colour
// helpers the software executor composes with. A pixel on screen is XRGB8888: 0x00RRGGBB.

#include <ceres/core/base/types.h>

namespace ceres::devices::video
{
	enum class PixelFormat : u32
	{
		I1 = 0,        // 1 bit a pixel, an index into the palette; the leftmost pixel is the byte's top bit
		I2 = 1,
		I4 = 2,
		I8 = 3,
		Rgb565 = 4,    // 16 bits: red 15:11, green 10:5, blue 4:0
		Argb1555 = 5,  // 16 bits: alpha 15, red 14:10, green 9:5, blue 4:0
		Xrgb8888 = 6,  // 32 bits: 0x00RRGGBB (the top byte is ignored)
		Argb8888 = 7,  // 32 bits: 0xAARRGGBB
	};

	inline constexpr u32 PixelFormatCount = 8;

	constexpr bool isPixelFormat(u32 code) noexcept { return code < PixelFormatCount; }

	constexpr u32 bitsPerPixel(PixelFormat format) noexcept
	{
		switch (format)
		{
			case PixelFormat::I1: return 1;
			case PixelFormat::I2: return 2;
			case PixelFormat::I4: return 4;
			case PixelFormat::I8: return 8;
			case PixelFormat::Rgb565:
			case PixelFormat::Argb1555: return 16;
			case PixelFormat::Xrgb8888:
			case PixelFormat::Argb8888: return 32;
		}
		return 32;
	}

	constexpr bool isIndexed(PixelFormat format) noexcept { return static_cast<u32>(format) <= static_cast<u32>(PixelFormat::I8); }

	// Widens a 5- or 6-bit channel to 8 bits the usual way: the top bits repeat in the bottom ones, so full is 255.
	constexpr u32 expand5(u32 v) noexcept { return (v << 3) | (v >> 2); }
	constexpr u32 expand6(u32 v) noexcept { return (v << 2) | (v >> 4); }

	constexpr u32 fromRgb565(u32 p) noexcept
	{
		return (expand5((p >> 11) & 31) << 16) | (expand6((p >> 5) & 63) << 8) | expand5(p & 31);
	}

	constexpr u32 fromArgb1555(u32 p) noexcept
	{
		return ((p & 0x8000) ? 0xFF000000u : 0u) | (expand5((p >> 10) & 31) << 16) | (expand5((p >> 5) & 31) << 8) | expand5(p & 31);
	}

	// `src` (0xAARRGGBB) over `dst` (0x00RRGGBB), in integers with rounding, so every host gets the same bytes.
	constexpr u32 blendOver(u32 src, u32 dst) noexcept
	{
		const u32 a = src >> 24;
		if (a == 255)
			return src & 0x00FFFFFF;
		if (a == 0)
			return dst & 0x00FFFFFF;
		u32 out = 0;
		for (u32 shift = 0; shift <= 16; shift += 8)
		{
			const u32 s = (src >> shift) & 255;
			const u32 d = (dst >> shift) & 255;
			out |= ((s * a + d * (255 - a) + 127) / 255) << shift;
		}
		return out;
	}
}

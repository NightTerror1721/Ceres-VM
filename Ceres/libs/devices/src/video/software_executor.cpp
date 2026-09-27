#include <ceres/devices/video/software_executor.h>
#include <ceres/devices/video/bitmap_plane.h>
#include <ceres/devices/video/formats.h>
#include <ceres/devices/video/text_plane.h>

#include <algorithm>
#include <array>

namespace ceres::devices::video
{
	namespace
	{
		// Reads a word of VRAM at a physical address, or 0 when the VRAM does not back it.
		u32 vramWord(const vm::Vram& vram, u32 address) noexcept
		{
			return vram.backs(address, 4) ? vram.read<u32>(address - vm::Vram::BaseValue) : 0u;
		}

		u8 vramByte(const vm::Vram& vram, u32 address) noexcept
		{
			return vram.backs(address, 1) ? vram.read<u8>(address - vm::Vram::BaseValue) : u8{ 0 };
		}

		// One pixel of a row of the bitmap plane, as 0xAARRGGBB (the formats without alpha are opaque).
		u32 bitmapPixel(PixelFormat format, const u8* row, u32 x, const std::array<u32, 256>& palette) noexcept
		{
			switch (format)
			{
				case PixelFormat::I1: return 0xFF000000u | palette[(row[x >> 3] >> (7 - (x & 7))) & 1u];
				case PixelFormat::I2: return 0xFF000000u | palette[(row[x >> 2] >> (6 - 2 * (x & 3))) & 3u];
				case PixelFormat::I4: return 0xFF000000u | palette[(row[x >> 1] >> (4 - 4 * (x & 1))) & 15u];
				case PixelFormat::I8: return 0xFF000000u | palette[row[x]];
				case PixelFormat::Rgb565: return 0xFF000000u | fromRgb565(static_cast<u32>(row[2 * x] | (row[2 * x + 1] << 8)));
				case PixelFormat::Argb1555: return fromArgb1555(static_cast<u32>(row[2 * x] | (row[2 * x + 1] << 8)));
				case PixelFormat::Xrgb8888:
				case PixelFormat::Argb8888:
				{
					const u32 p = static_cast<u32>(row[4 * x]) | (static_cast<u32>(row[4 * x + 1]) << 8) |
						(static_cast<u32>(row[4 * x + 2]) << 16) | (static_cast<u32>(row[4 * x + 3]) << 24);
					return format == PixelFormat::Xrgb8888 ? (0xFF000000u | p) : p;
				}
			}
			return 0;
		}

		// The bitmap plane over the background colour, scrolled and wrapping round (plan/v2 SPEC 7.1, V1). A row the
		// VRAM does not wholly back shows nothing.
		void composeBitmap(const BitmapPlane& bitmap, const vm::Vram& vram, VideoFrame& frame)
		{
			const u32 width = bitmap.width();
			const u32 height = bitmap.height();
			if (width == 0 || height == 0 || !isPixelFormat(bitmap.format()))
				return;
			const PixelFormat format = static_cast<PixelFormat>(bitmap.format());
			const u32 rowBytes = static_cast<u32>((u64{ width } * bitsPerPixel(format) + 7) / 8);

			std::array<u32, 256> palette{};
			if (isIndexed(format))
				for (u32 i = 0; i < palette.size(); ++i)
					palette[i] = vramWord(vram, bitmap.shownPalette() + i * 4) & 0x00FFFFFFu;

			for (u32 y = 0; y < frame.height; ++y)
			{
				const u32 sourceY = static_cast<u32>((u64{ y } + bitmap.scrollY()) % height);
				const u64 rowAddress = u64{ bitmap.shownBase() } + u64{ sourceY } * bitmap.pitch();
				if (rowAddress > 0xFFFFFFFFull || !vram.backs(static_cast<u32>(rowAddress), rowBytes))
					continue;
				const u8* row = vram.data() + (static_cast<u32>(rowAddress) - vm::Vram::BaseValue);
				u32* out = frame.pixels.data() + static_cast<usize>(y) * frame.width;
				u32 sourceX = bitmap.scrollX() % width;
				for (u32 x = 0; x < frame.width; ++x)
				{
					const u32 pixel = bitmapPixel(format, row, sourceX, palette);
					out[x] = blendOver(pixel, out[x]);
					if (++sourceX == width)
						sourceX = 0;
				}
			}
		}

		// The text plane over whatever is already in the frame (plan/v2 SPEC 7.2: it is the front of the order).
		void composeText(const ScanoutState& state, const TextPlane& text, const vm::Vram& vram, VideoFrame& frame)
		{
			std::array<u32, 256> palette;
			for (u32 i = 0; i < palette.size(); ++i)
				palette[i] = vramWord(vram, text.paletteBase() + i * 4) & 0x00FFFFFFu;

			const u32 cols = std::min(text.cols(), frame.width / TextPlane::CellWidth);
			const u32 rows = std::min(text.rows(), frame.height / TextPlane::CellHeight);
			const bool wide = text.cellFormat() == 1;
			const u32 shape = text.cursorShape() & 3u;
			const bool blinkOn = (text.cursorShape() & TextPlane::CursorBlink) == 0 || (state.frameCounter / TextPlane::BlinkFrames) % 2 == 0;
			const bool cursorShown = shape != TextPlane::CursorNone && blinkOn && text.scrollY() == 0;

			for (u32 row = 0; row < rows; ++row)
				for (u32 col = 0; col < cols; ++col)
				{
					const u32 cell = text.shownCell(vram, col, row);
					const u32 glyph = cell & 0xFF;
					u32 ink = wide ? (cell >> 8) & 0xFF : (cell >> 8) & 0x0F;
					u32 background = wide ? (cell >> 16) & 0xFF : (cell >> 12) & 0x0F;
					const bool underline = wide && (cell & (1u << 24)) != 0;
					if (wide && (cell & (1u << 25)) != 0)
						std::swap(ink, background);

					const bool cursorHere = cursorShown && col == text.cursorX() && row == text.cursorY();
					bool opaque = background != 0;
					if (cursorHere && shape == TextPlane::CursorBlock)
					{
						std::swap(ink, background);
						opaque = true;
					}

					const u32 glyphAddress = text.fontBase() + glyph * 16;
					const bool drawable = glyph < text.glyphCount();
					for (u32 y = 0; y < TextPlane::CellHeight; ++y)
					{
						u8 bits = drawable ? vramByte(vram, glyphAddress + y) : u8{ 0 };
						if (underline && y == TextPlane::CellHeight - 1)
							bits = 0xFF;
						if (cursorHere && shape == TextPlane::CursorUnderline && y >= TextPlane::CellHeight - 2)
							bits = 0xFF;
						if (cursorHere && shape == TextPlane::CursorBar)
							bits = static_cast<u8>(bits | 0xC0);
						u32* line = frame.pixels.data() + static_cast<usize>(row * TextPlane::CellHeight + y) * frame.width + col * TextPlane::CellWidth;
						for (u32 x = 0; x < TextPlane::CellWidth; ++x)
						{
							if ((bits & (0x80u >> x)) != 0)
								line[x] = palette[ink];
							else if (opaque)
								line[x] = palette[background];
						}
					}
				}
		}
	}

	void SoftwareExecutor::compose(const ScanoutState& state, const vm::Vram& vram, VideoFrame& frame)
	{
		frame.width = state.width;
		frame.height = state.height;
		frame.number = state.frameCounter;
		frame.pixels.assign(static_cast<usize>(state.width) * state.height, 0u);
		if (!state.displayOn)
			return;

		std::fill(frame.pixels.begin(), frame.pixels.end(), state.background & 0x00FFFFFFu);
		if (state.mode >= 1 && state.bitmap != nullptr && state.bitmap->enabled())
			composeBitmap(*state.bitmap, vram, frame);
		if (state.text != nullptr && state.text->enabled())
			composeText(state, *state.text, vram, frame);
	}
}

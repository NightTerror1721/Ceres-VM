#pragma once

// The text framebuffer as pixels, for a window: each character cell is 8 x 16 pixels of a bitmap font, in the
// cell's colours. Pure - a frame in, 0x00RRGGBB pixels out - so what a window shows can be checked without
// one, and the host only has to upload the result.
//
// The glyphs are the standard library's 5x7 dot-matrix shapes (default_font.h), each dot drawn two pixels tall and
// one wide, centred in the cell with a pixel of margin above, below and to the left. The strokes that build a
// box (| - _ =) run right to the edge of the cell instead, so the frames a text interface draws with them join up
// rather than showing as dashes. A + is a corner or a crossing: it reaches only toward the neighbours that a
// stroke can join (a - or + on either side, a | or + above and below), so a corner is a corner and a lone + is a plus.
//
// Colours: attribute 0 is the terminal's own (light grey on black); otherwise the low nibble is the foreground and
// the high nibble the background, in the ANSI order (0 black, 1 red, 2 green, 3 yellow, 4 blue, 5 magenta,
// 6 cyan, 7 white, 8-15 the bright versions). A cell's byte is Latin-1: printable ASCII and 0xA0-0xFF (U+00A0 to
// U+00FF, the accented letters of the western European languages) have glyphs; a control (below 0x20, or 0x7F to
// 0x9F) is a space, as on the terminal.

#include <ceres/devices/video/text_framebuffer.h>
#include <ceres/devices/video/default_font.h>

#include <vector>

namespace ceres::devices
{
	class TextRenderer
	{
	private:
		static bool joinsHorizontally(u8 c) noexcept { return c == '-' || c == '+'; }
		static bool joinsVertically(u8 c) noexcept { return c == '|' || c == '+'; }

		// Which sides of the + at (row, column) have a stroke to join.
		static u32 armsOf(const FramebufferDevice::Frame& frame, u32 row, u32 column) noexcept;

	public:
		static inline constexpr u32 CellWidth = 8;
		static inline constexpr u32 CellHeight = 16;
		static inline constexpr u32 DefaultForeground = 7;
		static inline constexpr u32 DefaultBackground = 0;

		// The per-pixel helpers below stay here, constexpr: the renderer calls them for every pixel of every cell,
		// and the compiler can fold them into its loops only when it sees their bodies.

		// 0x00RRGGBB for an entry of the 16-colour palette: the classic VGA one (yellow is brown, the second
		// eight are the bright ones).
		static constexpr u32 colour(u32 index) noexcept
		{
			constexpr u32 palette[16] = {
				0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
				0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF };
			return palette[index & 15u];
		}

		// Whether a cell's byte is a character with a glyph (printable ASCII or Latin-1), not a control.
		static constexpr bool printable(u8 c) noexcept { return (c >= 0x20 && c < 0x7F) || c >= 0xA0; }

		// Whether pixel (x, y) of a cell, 0 <= x < 8 and 0 <= y < 16, is part of character `c`.
		static constexpr bool glyphPixel(u8 c, u32 x, u32 y) noexcept
		{
			if (!printable(c) || x >= CellWidth || y >= CellHeight)
				return false;
			switch (c)
			{
				case '|': return x == 3 || x == 4;
				case '-': return y == 7 || y == 8;
				case '_': return y == 14 || y == 15;
				case '=': return y == 5 || y == 6 || y == 9 || y == 10;
				default: break;
			}
			return glyphPixelFont(c, x, y);
		}

		// The character as the font draws it, with none of the box strokes' stretching.
		static constexpr bool glyphPixelFont(u8 c, u32 x, u32 y) noexcept
		{
			if (!printable(c) || x < 1 || x > 5 || y < 1 || y > 14)
				return false;
			return ((TextFontGlyphs[c][(y - 1) / 2] >> (x - 1)) & 1u) != 0;
		}

		// The arms a + reaches out with, as bits of ArmLeft..ArmDown. With none, it is the font's own plus.
		static inline constexpr u32 ArmLeft = 1, ArmRight = 2, ArmUp = 4, ArmDown = 8;

		// Whether pixel (x, y) of a + cell is lit, given which neighbours it joins.
		static constexpr bool plusPixel(u32 arms, u32 x, u32 y) noexcept
		{
			if (arms == 0)
				return glyphPixelFont('+', x, y);
			if (x >= CellWidth || y >= CellHeight)
				return false;
			const bool horizontal = (y == 7 || y == 8) && (((arms & ArmLeft) != 0 && x <= 4) || ((arms & ArmRight) != 0 && x >= 3));
			const bool vertical = (x == 3 || x == 4) && (((arms & ArmUp) != 0 && y <= 8) || ((arms & ArmDown) != 0 && y >= 7));
			const bool centre = (x == 3 || x == 4) && (y == 7 || y == 8);
			return horizontal || vertical || centre;
		}

		static constexpr u32 imageWidth(const FramebufferDevice::Frame& frame) noexcept { return frame.width * CellWidth; }
		static constexpr u32 imageHeight(const FramebufferDevice::Frame& frame) noexcept { return frame.height * CellHeight; }

		// Draws the frame into `pixels` (resized to imageWidth x imageHeight, row by row).
		static void render(const FramebufferDevice::Frame& frame, std::vector<u32>& pixels);
	};
}

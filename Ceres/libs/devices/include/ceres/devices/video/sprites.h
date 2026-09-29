#pragma once

// The sprites of V2 (plan/v2 SPEC 7.5): 128 entries of 16 bytes in VRAM, the OAM, each a picture of 8 to 64 pixels a
// side placed anywhere on the screen, flipped, at one of the four priorities among the layers. A line shows the
// visible sprites that touch it in OAM order, up to the profile's limit; the rest are left out of that line, and the
// GPU says so in SpriteStatus. Both the scanout and that status count them here.

#include <ceres/core/base/types.h>
#include <ceres/vm/vram.h>

#include <span>
#include <vector>

namespace ceres::devices::video
{
	// An entry of the OAM. Word 0 holds the position, word 1 the rest; words 2 and 3 are V3's (F10).
	struct OamEntry
	{
		static inline constexpr u32 Size = 16;
		static inline constexpr u32 Count = 128;

		static inline constexpr u32 GraphicMask = 0xFFFFu;       // bits 15:0: the picture, at SpriteTileBase + this * 32
		static inline constexpr u32 GraphicUnit = 32;
		static inline constexpr u32 PaletteShift = 16;           // bits 19:16: the palette bank, in 4 bpp
		static inline constexpr u32 WidthShift = 20;             // bits 21:20: 8, 16, 32 or 64 pixels
		static inline constexpr u32 HeightShift = 22;            // bits 23:22
		static inline constexpr u32 FlipX = 1u << 24;
		static inline constexpr u32 FlipY = 1u << 25;
		static inline constexpr u32 PriorityShift = 26;          // bits 27:26
		static inline constexpr u32 Bpp8 = 1u << 28;
		static inline constexpr u32 Visible = 1u << 31;
	};

	// A visible sprite, as its entry says.
	struct Sprite
	{
		u32 index = 0;        // in the OAM: the lower is in front where two meet
		i32 x = 0;            // the top-left corner on the screen
		i32 y = 0;
		u32 width = 8;
		u32 height = 8;
		u32 graphic = 0;
		u32 bank = 0;
		u32 priority = 0;
		bool flipX = false;
		bool flipY = false;
		bool bpp8 = false;

		bool covers(u32 line) const noexcept { return i64{ line } >= y && i64{ line } < i64{ y } + height; }
	};

	// The visible sprites of the OAM at `oamBase`, in OAM order, into `out`. An entry outside the VRAM reads as 0 - not
	// visible.
	void readOam(const vm::Vram& vram, u32 oamBase, std::vector<Sprite>& out);

	// The sprites line `line` shows, in OAM order and no more than `limit`, into `out`: true when more touched it.
	bool spritesOnLine(std::span<const Sprite> sprites, u32 line, u32 limit, std::vector<const Sprite*>& out);

	struct SpriteOverflow
	{
		bool overflowed = false;
		u32 firstLine = 0;
	};

	// Whether any of lines [first, end) is touched by more than `limit` of `sprites`, and the first that is.
	SpriteOverflow findOverflow(std::span<const Sprite> sprites, u32 first, u32 end, u32 limit);
}

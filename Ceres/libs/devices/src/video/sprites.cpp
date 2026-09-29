#include <ceres/devices/video/sprites.h>

#include <algorithm>

namespace ceres::devices::video
{
	void readOam(const vm::Vram& vram, u32 oamBase, std::vector<Sprite>& out)
	{
		out.clear();
		for (u32 i = 0; i < OamEntry::Count; ++i)
		{
			const u64 at = u64{ oamBase } + u64{ i } * OamEntry::Size;
			if (at > 0xFFFFFFFFull || !vram.backs(static_cast<u32>(at), 8))
				continue;
			const u32 offset = static_cast<u32>(at) - vm::Vram::BaseValue;
			const u32 position = vram.read<u32>(offset);
			const u32 attributes = vram.read<u32>(offset + 4);
			if ((attributes & OamEntry::Visible) == 0)
				continue;
			out.push_back(Sprite{
				.index = i,
				.x = static_cast<i16>(static_cast<u16>(position & 0xFFFFu)),
				.y = static_cast<i16>(static_cast<u16>(position >> 16)),
				.width = 8u << ((attributes >> OamEntry::WidthShift) & 3u),
				.height = 8u << ((attributes >> OamEntry::HeightShift) & 3u),
				.graphic = attributes & OamEntry::GraphicMask,
				.bank = (attributes >> OamEntry::PaletteShift) & 15u,
				.priority = (attributes >> OamEntry::PriorityShift) & 3u,
				.flipX = (attributes & OamEntry::FlipX) != 0,
				.flipY = (attributes & OamEntry::FlipY) != 0,
				.bpp8 = (attributes & OamEntry::Bpp8) != 0,
			});
		}
	}

	bool spritesOnLine(std::span<const Sprite> sprites, u32 line, u32 limit, std::vector<const Sprite*>& out)
	{
		out.clear();
		for (const Sprite& sprite : sprites)
		{
			if (!sprite.covers(line))
				continue;
			if (out.size() == limit)
				return true;
			out.push_back(&sprite);
		}
		return false;
	}

	// Each sprite adds one to the lines it covers, through a table of differences: a pass over the sprites and one over
	// the lines, however tall the sprites are.
	SpriteOverflow findOverflow(std::span<const Sprite> sprites, u32 first, u32 end, u32 limit)
	{
		if (end <= first || sprites.size() <= limit)
			return {};
		std::vector<i32> starts(end - first + 1, 0);
		for (const Sprite& sprite : sprites)
		{
			const i64 top = std::max<i64>(sprite.y, first);
			const i64 bottom = std::min<i64>(i64{ sprite.y } + sprite.height, end);
			if (top >= bottom)
				continue;
			++starts[static_cast<usize>(top - first)];
			--starts[static_cast<usize>(bottom - first)];
		}
		i64 count = 0;
		for (u32 line = first; line < end; ++line)
		{
			count += starts[line - first];
			if (count > limit)
				return SpriteOverflow{ true, line };
		}
		return {};
	}
}

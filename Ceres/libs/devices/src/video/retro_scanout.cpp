#include <ceres/devices/video/retro_scanout.h>

#include <algorithm>

namespace ceres::devices::video
{
	namespace
	{
		// The VRAM at a physical address (64 bits wide, so an address past the end of the space is simply not
		// there), or 0 where the VRAM does not back it.
		u32 vramWord(const vm::Vram& vram, u64 address) noexcept
		{
			return address <= 0xFFFFFFFFull && vram.backs(static_cast<u32>(address), 4) ? vram.read<u32>(static_cast<u32>(address) - vm::Vram::BaseValue) : 0u;
		}

		u32 vramHalf(const vm::Vram& vram, u64 address) noexcept
		{
			return address <= 0xFFFFFFFFull && vram.backs(static_cast<u32>(address), 2) ? vram.read<u16>(static_cast<u32>(address) - vm::Vram::BaseValue) : 0u;
		}

		u32 vramByte(const vm::Vram& vram, u64 address) noexcept
		{
			return address <= 0xFFFFFFFFull && vram.backs(static_cast<u32>(address), 1) ? vram.read<u8>(static_cast<u32>(address) - vm::Vram::BaseValue) : 0u;
		}

		// Pixel (x, y) of a tile or a sprite's picture, `width` pixels wide, at `base`: its colour index, 4 or 8 bits;
		// in 4 bits the left pixel of a byte is its high half.
		u32 pictureIndex(const vm::Vram& vram, u64 base, u32 width, u32 bitsPerPixel, u32 x, u32 y) noexcept
		{
			const u64 pixel = u64{ y } * width + x;
			if (bitsPerPixel == 8)
				return vramByte(vram, base + pixel);
			const u32 byte = vramByte(vram, base + pixel / 2);
			return (pixel & 1) != 0 ? byte & 15u : byte >> 4;
		}

		i32 low16(u32 word) noexcept { return static_cast<i16>(static_cast<u16>(word & 0xFFFFu)); }
		i32 high16(u32 word) noexcept { return static_cast<i16>(static_cast<u16>(word >> 16)); }

		// Pixel (fineX, fineY) of the tile a map entry names, as `layer` draws it: false when it is transparent, and
		// otherwise its colour and the priority it is drawn at.
		bool mapPixel(const TileLayer& layer, const std::array<u32, Retro2D::PaletteEntries>& palette, const vm::Vram& vram,
			u32 entry, u32 fineX, u32 fineY, u32& colour, u8& priority) noexcept
		{
			const u32 size = layer.tileSize();
			const u32 bpp = layer.bitsPerPixel();
			const u32 px = (entry & MapEntry::FlipX) != 0 ? size - 1 - fineX : fineX;
			const u32 py = (entry & MapEntry::FlipY) != 0 ? size - 1 - fineY : fineY;
			const u64 tile = u64{ layer.tileBase } + u64{ entry & MapEntry::TileMask } * layer.tileBytes();
			const u32 index = pictureIndex(vram, tile, size, bpp, px, py);
			if (index == 0)
				return false;
			const u32 entryBank = (entry >> MapEntry::PaletteShift) & MapEntry::PaletteMask;
			colour = palette[bpp == 8 ? index : ((entryBank + layer.paletteBank()) & 15u) * 16 + index];
			priority = static_cast<u8>(std::min<u32>(3, layer.priority() + ((entry & MapEntry::Priority) != 0 ? 1 : 0)));
			return true;
		}
	}

	void RetroScanout::beginFrame() noexcept
	{
		_tilePalette.loaded = false;
	}

	const RetroScanout::Palette& RetroScanout::palette(Palette& cache, u32 base, const vm::Vram& vram)
	{
		if (!cache.loaded || cache.base != base)
		{
			for (u32 i = 0; i < Retro2D::PaletteEntries; ++i)
				cache.colours[i] = vramWord(vram, u64{ base } + 4 * i) & 0x00FFFFFFu;
			cache.base = base;
			cache.loaded = true;
		}
		return cache;
	}

	// A tile layer's pixels on line y. The map repeats both ways; its size in pixels is a power of two, so the
	// scroll wraps with a mask whatever its sign.
	void RetroScanout::drawTileLayer(const TileLayer& layer, const Palette& palette, const vm::Vram& vram, u32 y, LayerLine& out)
	{
		const u32 size = layer.tileSize();
		const u32 mapWidth = layer.mapWidth();
		const u32 widthMask = mapWidth * size - 1;
		const u32 heightMask = layer.mapHeight() * size - 1;

		u32 dx = 0, dy = 0;
		if (layer.lineScroll())
		{
			const u32 shift = vramWord(vram, u64{ layer.lineScrollBase } + 4 * u64{ y });
			dx = static_cast<u32>(low16(shift));
			dy = static_cast<u32>(high16(shift));
		}
		const u32 mapY = (y + layer.scrollY + dy) & heightMask;
		const u32 row = mapY / size;
		const u32 fineY = mapY % size;
		u32 mapX = (layer.scrollX + dx) & widthMask;

		u32 column = ~0u;
		u32 entry = 0;
		for (usize x = 0; x < out.colours.size(); ++x, mapX = (mapX + 1) & widthMask)
		{
			if (mapX / size != column)
			{
				column = mapX / size;
				entry = vramHalf(vram, u64{ layer.mapBase } + 2 * (u64{ row } * mapWidth + column));
			}
			if (!mapPixel(layer, palette.colours, vram, entry, mapX % size, fineY, out.colours[x], out.priorities[x]))
				out.priorities[x] = Transparent;
		}
	}

	// The affine layer's pixels on line y: each is looked up through the matrix on its own. The coordinates are
	// worked out in 64 bits, where nothing overflows, and shifted arithmetically, so -0.5 is pixel -1.
	void RetroScanout::drawAffineLayer(const AffineLayer& affine, const Palette& palette, const vm::Vram& vram, u32 y, LayerLine& out)
	{
		const TileLayer layer = affine.asTileLayer();
		const u32 size = layer.tileSize();
		const u32 mapWidth = layer.mapWidth();
		const i64 widthPixels = i64{ mapWidth } * size;
		const i64 heightPixels = i64{ layer.mapHeight() } * size;
		const bool wraps = affine.wraps();
		i64 u = i64{ static_cast<i32>(affine.originX) } + i64{ affine.pb() } * y;
		i64 v = i64{ static_cast<i32>(affine.originY) } + i64{ affine.pd() } * y;
		for (usize x = 0; x < out.colours.size(); ++x, u += affine.pa(), v += affine.pc())
		{
			i64 mapX = u >> 8;
			i64 mapY = v >> 8;
			if (wraps)
			{
				mapX &= widthPixels - 1;
				mapY &= heightPixels - 1;
			}
			else if (mapX < 0 || mapY < 0 || mapX >= widthPixels || mapY >= heightPixels)
			{
				out.priorities[x] = Transparent;
				continue;
			}
			const u32 column = static_cast<u32>(mapX) / size;
			const u32 row = static_cast<u32>(mapY) / size;
			const u32 entry = vramHalf(vram, u64{ layer.mapBase } + 2 * (u64{ row } * mapWidth + column));
			if (!mapPixel(layer, palette.colours, vram, entry, static_cast<u32>(mapX) % size, static_cast<u32>(mapY) % size, out.colours[x], out.priorities[x]))
				out.priorities[x] = Transparent;
		}
	}

	void RetroScanout::composeLine(const Retro2D& retro, const vm::Vram& vram, u32 y, std::span<u32> line)
	{
		std::array<bool, LayerSlots> drawn{};
		for (u32 i = 0; i < Retro2D::LayerCount; ++i)
		{
			const TileLayer& layer = retro.layer(i);
			if (!layer.enabled())
				continue;
			LayerLine& out = _layers[i];
			out.colours.resize(line.size());
			out.priorities.resize(line.size());
			drawTileLayer(layer, palette(_tilePalette, retro.tilePaletteBase(), vram), vram, y, out);
			drawn[i] = true;
		}
		if (retro.affine().enabled())
		{
			LayerLine& out = _layers[AffineSlot];
			out.colours.resize(line.size());
			out.priorities.resize(line.size());
			drawAffineLayer(retro.affine(), palette(_tilePalette, retro.tilePaletteBase(), vram), vram, y, out);
			drawn[AffineSlot] = true;
		}

		// From the back: each priority in turn, and within one the affine layer, then the tile layers from 3 to 0.
		for (u8 priority = 0; priority <= 3; ++priority)
			for (u32 i = LayerSlots; i-- > 0;)
			{
				if (!drawn[i])
					continue;
				const LayerLine& layer = _layers[i];
				for (usize x = 0; x < line.size(); ++x)
					if (layer.priorities[x] == priority)
						line[x] = layer.colours[x];
			}
	}
}

#pragma once

// The GPU's Retro 2D level, V2 (plan/v2 SPEC 7.5): the registers at 0x300-0x3FF of slot 0x40, as the scanout reads
// them. Colour is indexed, as on the 8- and 16-bit consoles: a palette of 256 entries of 0x00RRGGBB in VRAM, which
// 4-bit graphics see as 16 banks of 16. Four tile layers draw maps of 16-bit entries (the tile, a palette, the flips
// and a priority bit) over tiles of 8x8 or 16x16 pixels, each layer scrolling on its own and, with a table in VRAM,
// line by line. A fifth, affine layer draws a map the same way through a 2x2 matrix: turned, scaled, sheared. 128
// sprites (sprites.h) go among the layers by priority, no more on a line than the machine's profile allows.
//
// These registers take effect at once - no Present - and the scanout reads them line by line (SPEC 7.6). It is a
// value type: nothing here reaches the VRAM, which the scanout reads when it composes.

#include <ceres/core/base/types.h>

#include <algorithm>
#include <array>

namespace ceres::devices::video
{
	// A layer's Control register (SPEC 7.5).
	struct LayerControl
	{
		static inline constexpr u32 Enable = 1u << 0;
		static inline constexpr u32 LargeTiles = 1u << 1;        // 16x16 tiles; 8x8 without it
		static inline constexpr u32 Bpp8 = 1u << 2;              // 8 bits a pixel; 4 without it
		static inline constexpr u32 PriorityShift = 4;           // bits 5:4, 0-3, 3 in front
		static inline constexpr u32 MapWidthShift = 6;           // bits 7:6: 32, 64, 128 or 128 tiles
		static inline constexpr u32 MapHeightShift = 8;          // bits 9:8, the same
		static inline constexpr u32 LineScroll = 1u << 10;       // the table of a scroll for every line
		static inline constexpr u32 Wrap = 1u << 11;             // the affine layer repeats its map; without it, clips
		static inline constexpr u32 PaletteBankShift = 12;       // bits 15:12, added to the entries' palettes
		static inline constexpr u32 Mask = 0xFFFFu;
	};

	// An entry of a tile map: 16 bits.
	struct MapEntry
	{
		static inline constexpr u32 TileMask = 0x3FFu;           // bits 9:0
		static inline constexpr u32 PaletteShift = 10;           // bits 12:10, added to the layer's bank
		static inline constexpr u32 PaletteMask = 0x7u;
		static inline constexpr u32 FlipX = 1u << 13;
		static inline constexpr u32 FlipY = 1u << 14;
		static inline constexpr u32 Priority = 1u << 15;         // one level in front of its layer (3 at most)
	};

	// One of the four tile layers, as its registers hold it.
	struct TileLayer
	{
		u32 control = 0;
		u32 mapBase = 0;
		u32 tileBase = 0;
		u32 scrollX = 0;
		u32 scrollY = 0;
		u32 lineScrollBase = 0;

		bool enabled() const noexcept { return (control & LayerControl::Enable) != 0; }
		u32 tileSize() const noexcept { return (control & LayerControl::LargeTiles) != 0 ? 16u : 8u; }
		u32 bitsPerPixel() const noexcept { return (control & LayerControl::Bpp8) != 0 ? 8u : 4u; }
		u32 tileBytes() const noexcept { return tileSize() * tileSize() * bitsPerPixel() / 8; }
		u32 priority() const noexcept { return (control >> LayerControl::PriorityShift) & 3u; }
		// The map's size in tiles: 32, 64 or 128 (the code 3 is 128 too).
		u32 mapWidth() const noexcept { return mapTiles(control >> LayerControl::MapWidthShift); }
		u32 mapHeight() const noexcept { return mapTiles(control >> LayerControl::MapHeightShift); }
		bool lineScroll() const noexcept { return (control & LayerControl::LineScroll) != 0; }
		u32 paletteBank() const noexcept { return (control >> LayerControl::PaletteBankShift) & 15u; }

		bool operator==(const TileLayer&) const noexcept = default;

		static constexpr u32 mapTiles(u32 code) noexcept { return 32u << std::min<u32>(code & 3u, 2u); }
	};

	// The affine layer: a map like a tile layer's, seen through a matrix. Screen pixel (x, y) is map pixel
	// ((OriginX + pa*x + pb*y) >> 8, (OriginY + pc*x + pd*y) >> 8), the origin in 24.8 and the matrix in 8.8, all signed.
	struct AffineLayer
	{
		static inline constexpr u32 Identity = 0x100;   // 1.0 in 8.8

		u32 control = 0;
		u32 mapBase = 0;
		u32 tileBase = 0;
		u32 originX = 0;
		u32 originY = 0;
		u32 matrixAB = Identity;         // pa in bits 15:0, pb in 31:16
		u32 matrixCD = Identity << 16;   // pc in bits 15:0, pd in 31:16

		// The same fields a tile layer's Control has, read the same way.
		TileLayer asTileLayer() const noexcept { return TileLayer{ .control = control, .mapBase = mapBase, .tileBase = tileBase }; }
		bool enabled() const noexcept { return (control & LayerControl::Enable) != 0; }
		bool wraps() const noexcept { return (control & LayerControl::Wrap) != 0; }
		i32 pa() const noexcept { return static_cast<i16>(static_cast<u16>(matrixAB)); }
		i32 pb() const noexcept { return static_cast<i16>(static_cast<u16>(matrixAB >> 16)); }
		i32 pc() const noexcept { return static_cast<i16>(static_cast<u16>(matrixCD)); }
		i32 pd() const noexcept { return static_cast<i16>(static_cast<u16>(matrixCD >> 16)); }

		bool operator==(const AffineLayer&) const noexcept = default;
	};

	class Retro2D
	{
	public:
		static inline constexpr u32 TilePaletteBaseRegister = 0x300;     // RW: the layers' palette, 256 entries
		static inline constexpr u32 SpritePaletteBaseRegister = 0x304;   // RW: the sprites' palette
		static inline constexpr u32 OamBaseRegister = 0x308;             // RW: 128 sprites of 16 bytes
		static inline constexpr u32 SpriteTileBaseRegister = 0x30C;      // RW: their pictures, in units of 32 bytes
		static inline constexpr u32 SpriteControlRegister = 0x310;       // RW: bit 0 the sprites on
		static inline constexpr u32 SpriteStatusRegister = 0x314;        // R: bit 0 a line overflowed last frame, 31:16 the first
		static inline constexpr u32 SpriteLimitRegister = 0x318;         // R: sprites a line, the profile's
		static inline constexpr u32 LineTableBaseRegister = 0x31C;       // RW: the line table
		static inline constexpr u32 LineTableCountRegister = 0x320;      // RW: its entries, 0 off
		static inline constexpr u32 FirstRegister = 0x300;
		static inline constexpr u32 LastRegister = 0x3FC;

		// The tile layers: a block of 0x20 bytes each from 0x340.
		static inline constexpr u32 LayerCount = 4;
		static inline constexpr u32 LayerRegisters = 0x340;
		static inline constexpr u32 LayerStride = 0x20;
		static inline constexpr u32 LayerControlRegister = 0x00;         // offsets within a layer's block
		static inline constexpr u32 LayerMapBaseRegister = 0x04;
		static inline constexpr u32 LayerTileBaseRegister = 0x08;
		static inline constexpr u32 LayerScrollXRegister = 0x0C;
		static inline constexpr u32 LayerScrollYRegister = 0x10;
		static inline constexpr u32 LayerLineScrollBaseRegister = 0x14;

		// The affine layer.
		static inline constexpr u32 AffineControlRegister = 0x3C0;
		static inline constexpr u32 AffineMapBaseRegister = 0x3C4;
		static inline constexpr u32 AffineTileBaseRegister = 0x3C8;
		static inline constexpr u32 AffineOriginXRegister = 0x3CC;       // signed 24.8
		static inline constexpr u32 AffineOriginYRegister = 0x3D0;
		static inline constexpr u32 AffineMatrixABRegister = 0x3D4;      // pa 15:0, pb 31:16, signed 8.8
		static inline constexpr u32 AffineMatrixCDRegister = 0x3D8;      // pc 15:0, pd 31:16

		static inline constexpr u32 PaletteEntries = 256;

		static inline constexpr u32 SpritesOn = 1u << 0;                 // SpriteControl
		static inline constexpr u32 SpriteOverflow = 1u << 0;            // SpriteStatus
		static inline constexpr u32 SpriteOverflowLineShift = 16;

		// The line table: entries of 8 bytes (SPEC 7.5), no more than this many.
		static inline constexpr u32 LineTableEntrySize = 8;
		static inline constexpr u32 MaxLineTableEntries = 8192;

		// The register of layer `layer` at `field` (one of the Layer*Register offsets).
		static constexpr u32 layerRegister(u32 layer, u32 field) noexcept { return LayerRegisters + layer * LayerStride + field; }

	private:
		u32 _tilePaletteBase = 0;
		u32 _spritePaletteBase = 0;
		u32 _oamBase = 0;
		u32 _spriteTileBase = 0;
		u32 _spriteControl = 0;
		u32 _spriteStatus = 0;
		u32 _spriteLimit = 128;
		u32 _lineTableBase = 0;
		u32 _lineTableCount = 0;
		std::array<TileLayer, LayerCount> _layers{};
		AffineLayer _affine;

	public:
		// Every register back to 0, and the affine layer's matrix to the identity; SpriteLimit reads `spriteLimit`.
		void reset(u32 spriteLimit) noexcept;

		static constexpr bool handles(u32 offset) noexcept { return offset >= FirstRegister && offset <= LastRegister; }
		u32 read(u32 offset) const noexcept;
		void write(u32 offset, u32 value) noexcept;

		u32 tilePaletteBase() const noexcept { return _tilePaletteBase; }
		u32 spritePaletteBase() const noexcept { return _spritePaletteBase; }
		u32 oamBase() const noexcept { return _oamBase; }
		u32 spriteTileBase() const noexcept { return _spriteTileBase; }
		bool spritesOn() const noexcept { return (_spriteControl & SpritesOn) != 0; }
		u32 spriteLimit() const noexcept { return _spriteLimit; }
		u32 spriteStatus() const noexcept { return _spriteStatus; }
		u32 lineTableBase() const noexcept { return _lineTableBase; }
		u32 lineTableCount() const noexcept { return _lineTableCount; }
		// The GPU's, at each vertical blank: what the frame's lines did with the limit.
		void setSpriteStatus(bool overflowed, u32 firstLine) noexcept
		{
			_spriteStatus = overflowed ? SpriteOverflow | (firstLine << SpriteOverflowLineShift) : 0;
		}
		const TileLayer& layer(u32 index) const noexcept { return _layers[index]; }
		const AffineLayer& affine() const noexcept { return _affine; }
		// Whether anything of V2 would show: a layer or the sprites on.
		bool anyEnabled() const noexcept;
		// Whether the line table may write the register at `offset` (SPEC 7.5): its own registers and the read-only ones
		// may not.
		static constexpr bool lineTableWrites(u32 offset) noexcept
		{
			return handles(offset) && offset != SpriteStatusRegister && offset != SpriteLimitRegister &&
				offset != LineTableBaseRegister && offset != LineTableCountRegister;
		}

		bool operator==(const Retro2D&) const noexcept = default;
	};
}

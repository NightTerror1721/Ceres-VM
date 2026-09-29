#include "device_test_machine.h"

#include <ceres/devices/video/gpu.h>

// The affine layer of V2 (plan/v2 SPEC 7.5): a map seen through a 2x2 matrix in 8.8 from an origin in 24.8. Turned,
// scaled, mirrored and sheared maps, repeating or clipped, are composed and compared pixel by pixel with a plain
// model written here, and the frames' hash is fixed.

namespace
{
	using video::LayerControl;
	using video::MapEntry;
	using video::Retro2D;

	u64 fnv1a(const std::vector<u32>& pixels)
	{
		u64 hash = 1469598103934665603ull;
		for (u32 pixel : pixels)
			for (u32 b = 0; b < 4; ++b)
			{
				hash ^= (pixel >> (8 * b)) & 0xFF;
				hash *= 1099511628211ull;
			}
		return hash;
	}

	void set(CeresVM& vm, u32 offset, u32 value) { vm.io().write(default_mmio::Gpu + Address(offset), value); }
	u32 get(CeresVM& vm, u32 offset) { return vm.io().read(default_mmio::Gpu + Address(offset)); }

	constexpr u32 PaletteAt = 0xA0100000;
	constexpr u32 MapAt = 0xA0110000;
	constexpr u32 TilesAt = 0xA0120000;
	constexpr u32 ScreenWidth = 64;
	constexpr u32 ScreenHeight = 32;
	constexpr u32 Background = 0x203040;

	struct Lcg
	{
		u32 state;
		u32 next() { state = state * 1664525u + 1013904223u; return state >> 8; }
	};

	struct Affine
	{
		u32 tileSize = 8;
		u32 bpp = 4;
		u32 mapCode = 0;
		bool wrap = false;
		i32 originX = 0, originY = 0;
		i32 pa = 0x100, pb = 0, pc = 0, pd = 0x100;
		std::vector<u16> map;
		std::vector<u8> tiles;
		std::vector<u32> palette;

		u32 mapTiles() const { return 32u << std::min(mapCode, 2u); }
		u32 tileBytes() const { return tileSize * tileSize * bpp / 8; }
		u32 control() const
		{
			return LayerControl::Enable | (tileSize == 16 ? LayerControl::LargeTiles : 0) | (bpp == 8 ? LayerControl::Bpp8 : 0) |
				(mapCode << LayerControl::MapWidthShift) | (mapCode << LayerControl::MapHeightShift) | (wrap ? LayerControl::Wrap : 0);
		}
	};

	void fill(Affine& layer, u32 seed)
	{
		Lcg random{ seed };
		layer.map.resize(layer.mapTiles() * layer.mapTiles());
		for (u16& entry : layer.map)
			entry = static_cast<u16>(random.next() % 24 | (random.next() & 0x7C00u));   // flips and palettes, no priority bit
		layer.tiles.resize(24 * layer.tileBytes());
		for (u8& byte : layer.tiles)
			byte = static_cast<u8>(random.next() % 4 == 0 ? 0 : random.next());
		layer.palette.resize(256);
		for (u32& colour : layer.palette)
			colour = random.next() & 0x00FFFFFFu;
	}

	void show(CeresVM& vm, const Affine& layer)
	{
		Vram& vram = vm.vram();
		for (usize i = 0; i < layer.map.size(); ++i)
			vram.write<u16>(MapAt - Vram::BaseValue + static_cast<u32>(2 * i), layer.map[i]);
		for (usize i = 0; i < layer.tiles.size(); ++i)
			vram.write<u8>(TilesAt - Vram::BaseValue + static_cast<u32>(i), layer.tiles[i]);
		for (usize i = 0; i < layer.palette.size(); ++i)
			vram.write<u32>(PaletteAt - Vram::BaseValue + static_cast<u32>(4 * i), layer.palette[i]);

		set(vm, GpuDevice::WidthRegister.value(), ScreenWidth);
		set(vm, GpuDevice::HeightRegister.value(), ScreenHeight);
		set(vm, GpuDevice::ModeRegister.value(), 2);
		set(vm, GpuDevice::BackgroundColorRegister.value(), Background);
		set(vm, video::TextPlane::EnableRegister, 0);
		set(vm, Retro2D::TilePaletteBaseRegister, PaletteAt);
		set(vm, Retro2D::AffineMapBaseRegister, MapAt);
		set(vm, Retro2D::AffineTileBaseRegister, TilesAt);
		set(vm, Retro2D::AffineOriginXRegister, static_cast<u32>(layer.originX));
		set(vm, Retro2D::AffineOriginYRegister, static_cast<u32>(layer.originY));
		set(vm, Retro2D::AffineMatrixABRegister, (static_cast<u32>(layer.pa) & 0xFFFF) | (static_cast<u32>(layer.pb) << 16));
		set(vm, Retro2D::AffineMatrixCDRegister, (static_cast<u32>(layer.pc) & 0xFFFF) | (static_cast<u32>(layer.pd) << 16));
		set(vm, Retro2D::AffineControlRegister, layer.control());
	}

	i64 floorDiv256(i64 value) { return value >= 0 ? value / 256 : -((-value + 255) / 256); }

	// The model, from SPEC 7.5.
	u32 expected(const Affine& layer, u32 x, u32 y)
	{
		const i64 size = i64{ layer.mapTiles() } * layer.tileSize;
		i64 u = floorDiv256(i64{ layer.originX } + i64{ layer.pa } * x + i64{ layer.pb } * y);
		i64 v = floorDiv256(i64{ layer.originY } + i64{ layer.pc } * x + i64{ layer.pd } * y);
		if (layer.wrap)
		{
			u = ((u % size) + size) % size;
			v = ((v % size) + size) % size;
		}
		else if (u < 0 || v < 0 || u >= size || v >= size)
			return Background;
		const u32 entry = layer.map[static_cast<usize>((v / layer.tileSize) * layer.mapTiles() + u / layer.tileSize)];
		u32 px = static_cast<u32>(u % layer.tileSize);
		u32 py = static_cast<u32>(v % layer.tileSize);
		if (entry & MapEntry::FlipX)
			px = layer.tileSize - 1 - px;
		if (entry & MapEntry::FlipY)
			py = layer.tileSize - 1 - py;
		const usize tile = (entry & 0x3FF) * layer.tileBytes();
		const usize pixel = py * layer.tileSize + px;
		u32 index;
		if (layer.bpp == 8)
			index = layer.tiles[tile + pixel];
		else
		{
			const u8 byte = layer.tiles[tile + pixel / 2];
			index = pixel % 2 == 0 ? byte >> 4 : byte & 15;
		}
		if (index == 0)
			return Background;
		return layer.bpp == 8 ? layer.palette[index] : layer.palette[((entry >> 10) & 7) * 16 + index];
	}
}

TEST(affine_layer, turned_scaled_and_sheared_maps_match_the_model)
{
	// cos 30 and sin 30 in 8.8: 222 and 128.
	struct Case
	{
		u32 tileSize, bpp, mapCode;
		bool wrap;
		i32 originX, originY, pa, pb, pc, pd;
	};
	const Case cases[] = {
		{ 8, 4, 0, false, 0, 0, 0x100, 0, 0, 0x100 },                       // the identity
		{ 8, 4, 0, true, -40 * 256, 1000 * 256 + 128, 0x100, 0, 0, 0x100 },  // moved, repeating
		{ 8, 8, 1, true, 3000, -7000, 222, -128, 128, 222 },               // turned by 30 degrees
		{ 8, 8, 1, false, 100 * 256, 20 * 256, 222, 128, -128, 222 },       // the other way, clipped
		{ 16, 4, 0, false, 0, 0, 0x80, 0, 0, 0x80 },                       // twice as big
		{ 16, 4, 2, true, 50, 60, 0x300, 0, 0, 0x280 },                    // smaller, unevenly
		{ 16, 8, 1, true, 0, 0, -0x100, 0, 0, 0x100 },                     // mirrored
		{ 16, 8, 2, false, 8000, 9000, 0x100, 0x40, 0x20, 0x100 },          // sheared
		{ 8, 4, 2, true, 0x7FFFFF00, -0x7FFFFF00, 0x7FFF, -0x8000, -0x8000, 0x7FFF },   // the extremes
		{ 8, 8, 3, false, -300, -300, 0x101, 0x3, -0x5, 0xFF },
	};
	u64 combined = 0;
	u32 seed = 100;
	for (const Case& c : cases)
	{
		CeresVM vm;
		GpuDevice gpu;
		gpu.attachTo(vm.io());
		Affine layer;
		layer.tileSize = c.tileSize;
		layer.bpp = c.bpp;
		layer.mapCode = c.mapCode;
		layer.wrap = c.wrap;
		layer.originX = c.originX;
		layer.originY = c.originY;
		layer.pa = c.pa;
		layer.pb = c.pb;
		layer.pc = c.pc;
		layer.pd = c.pd;
		fill(layer, seed++);
		show(vm, layer);
		video::VideoFrame frame;
		gpu.compose(frame);
		u32 wrong = 0;
		u32 shown = 0;
		for (u32 y = 0; y < ScreenHeight; ++y)
			for (u32 x = 0; x < ScreenWidth; ++x)
			{
				const u32 model = expected(layer, x, y);
				if (frame.pixels[y * ScreenWidth + x] != model)
					++wrong;
				if (model != Background)
					++shown;
			}
		CHECK_EQ(wrong, 0u);
		CHECK(shown > 0);
		combined = combined * 31 + fnv1a(frame.pixels);
		gpu.detachFrom(vm.io());
	}
	CHECK_EQ(combined, 1860410338824634601ull);   // every case's whole frame, fixed
}

TEST(affine_layer, it_starts_as_the_identity_and_sits_behind_the_tile_layers)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	CHECK_EQ(get(vm, Retro2D::AffineMatrixABRegister), 0x100u);
	CHECK_EQ(get(vm, Retro2D::AffineMatrixCDRegister), 0x1000000u);
	set(vm, Retro2D::AffineMatrixABRegister, 0x12345678);
	set(vm, Retro2D::AffineControlRegister, 0xFFFFFFFFu);
	CHECK_EQ(get(vm, Retro2D::AffineControlRegister), 0xFFFFu);
	set(vm, GpuDevice::ControlRegister.value(), GpuDevice::ControlReset);
	CHECK_EQ(get(vm, Retro2D::AffineMatrixABRegister), 0x100u);
	CHECK_EQ(get(vm, Retro2D::AffineControlRegister), 0u);

	// Tile 1 is colour 1 all over; bank n's colour 1 is 0x0000n1. The affine map uses bank 1, layer 3's bank 3; both
	// at priority 0, layer 3 is in front; with the affine layer at priority 1, it is.
	Vram& vram = vm.vram();
	for (u32 i = 0; i < 32; ++i)
		vram.write<u8>(TilesAt - Vram::BaseValue + 32 + i, 0x11);
	for (u32 bank = 0; bank < 16; ++bank)
		vram.write<u32>(PaletteAt - Vram::BaseValue + 4 * (bank * 16 + 1), bank * 0x10 + 1);
	for (u32 i = 0; i < 32 * 32; ++i)
	{
		vram.write<u16>(MapAt - Vram::BaseValue + 2 * i, static_cast<u16>(1u | (1u << MapEntry::PaletteShift)));
		vram.write<u16>(MapAt - Vram::BaseValue + 0x800 + 2 * i, static_cast<u16>(1u | (3u << MapEntry::PaletteShift)));
	}
	Affine none;
	show(vm, none);
	set(vm, Retro2D::AffineControlRegister, LayerControl::Enable);
	set(vm, Retro2D::layerRegister(3, Retro2D::LayerMapBaseRegister), MapAt + 0x800);
	set(vm, Retro2D::layerRegister(3, Retro2D::LayerTileBaseRegister), TilesAt);
	set(vm, Retro2D::layerRegister(3, Retro2D::LayerControlRegister), LayerControl::Enable);
	video::VideoFrame frame;
	gpu.compose(frame);
	CHECK_EQ(frame.pixels[0], 0x31u);
	set(vm, Retro2D::AffineControlRegister, LayerControl::Enable | (1u << LayerControl::PriorityShift));
	gpu.compose(frame);
	CHECK_EQ(frame.pixels[0], 0x11u);
	gpu.detachFrom(vm.io());
}

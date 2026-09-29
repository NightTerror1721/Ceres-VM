#include "device_test_machine.h"

#include <ceres/devices/video/gpu.h>

// The tile layers of V2 (plan/v2 SPEC 7.5). Every combination of tile size, bits a pixel and map size is composed and
// compared, pixel by pixel, with a plain model of the same rules written here, and the frames' hash is fixed.

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
	void setLayer(CeresVM& vm, u32 layer, u32 field, u32 value) { set(vm, Retro2D::layerRegister(layer, field), value); }

	constexpr u32 PaletteAt = 0xA0100000;
	constexpr u32 MapAt = 0xA0110000;
	constexpr u32 TilesAt = 0xA0120000;
	constexpr u32 LineScrollAt = 0xA0170000;
	constexpr u32 ScreenWidth = 64;
	constexpr u32 ScreenHeight = 32;
	constexpr u32 Background = 0x102030;

	struct Lcg
	{
		u32 state;
		u32 next() { state = state * 1664525u + 1013904223u; return state >> 8; }
	};

	// A layer as the test sets it up, and what it wrote into the VRAM, for the model to read back.
	struct Layer
	{
		u32 tileSize = 8;
		u32 bpp = 4;
		u32 mapCode = 0;       // bits 7:6 of Control
		u32 mapHeightCode = 0; // bits 9:8
		u32 scrollX = 0;
		u32 scrollY = 0;
		u32 bank = 0;
		bool lineScroll = false;
		std::vector<u16> map;
		std::vector<u8> tiles;
		std::vector<u32> palette;
		std::vector<u32> shifts;   // a word a line of the screen

		u32 mapWidth() const { return 32u << std::min(mapCode, 2u); }
		u32 mapHeight() const { return 32u << std::min(mapHeightCode, 2u); }
		u32 tileBytes() const { return tileSize * tileSize * bpp / 8; }
		u32 control() const
		{
			return LayerControl::Enable | (tileSize == 16 ? LayerControl::LargeTiles : 0) | (bpp == 8 ? LayerControl::Bpp8 : 0) |
				(mapCode << LayerControl::MapWidthShift) | (mapHeightCode << LayerControl::MapHeightShift) |
				(lineScroll ? LayerControl::LineScroll : 0) | (bank << LayerControl::PaletteBankShift);
		}
	};

	// Fills the layer's map, 40 tiles, the palette and the line shifts with numbers from `seed`.
	void fill(Layer& layer, u32 seed)
	{
		Lcg random{ seed };
		layer.map.resize(layer.mapWidth() * layer.mapHeight());
		for (u16& entry : layer.map)
			entry = static_cast<u16>(random.next() % 40 | (random.next() & 0xFC00u));
		layer.tiles.resize(40 * layer.tileBytes());
		for (u8& byte : layer.tiles)
			byte = static_cast<u8>(random.next() % 3 == 0 ? 0 : random.next());   // a third of the pixels, or more, transparent
		layer.palette.resize(256);
		for (u32& colour : layer.palette)
			colour = random.next() & 0x00FFFFFFu;
		layer.shifts.resize(ScreenHeight);
		for (u32& shift : layer.shifts)
			shift = (random.next() & 0x00FF00FFu) - 0x00800080u;   // dx and dy from -128 to 127
	}

	void upload(CeresVM& vm, const Layer& layer)
	{
		Vram& vram = vm.vram();
		for (usize i = 0; i < layer.map.size(); ++i)
			vram.write<u16>(MapAt - Vram::BaseValue + static_cast<u32>(2 * i), layer.map[i]);
		for (usize i = 0; i < layer.tiles.size(); ++i)
			vram.write<u8>(TilesAt - Vram::BaseValue + static_cast<u32>(i), layer.tiles[i]);
		for (usize i = 0; i < layer.palette.size(); ++i)
			vram.write<u32>(PaletteAt - Vram::BaseValue + static_cast<u32>(4 * i), layer.palette[i]);
		for (usize i = 0; i < layer.shifts.size(); ++i)
			vram.write<u32>(LineScrollAt - Vram::BaseValue + static_cast<u32>(4 * i), layer.shifts[i]);
	}

	void show(CeresVM& vm, const Layer& layer)
	{
		set(vm, GpuDevice::WidthRegister.value(), ScreenWidth);
		set(vm, GpuDevice::HeightRegister.value(), ScreenHeight);
		set(vm, GpuDevice::ModeRegister.value(), 2);
		set(vm, GpuDevice::BackgroundColorRegister.value(), Background);
		set(vm, video::TextPlane::EnableRegister, 0);
		set(vm, Retro2D::TilePaletteBaseRegister, PaletteAt);
		setLayer(vm, 0, Retro2D::LayerMapBaseRegister, MapAt);
		setLayer(vm, 0, Retro2D::LayerTileBaseRegister, TilesAt);
		setLayer(vm, 0, Retro2D::LayerScrollXRegister, layer.scrollX);
		setLayer(vm, 0, Retro2D::LayerScrollYRegister, layer.scrollY);
		setLayer(vm, 0, Retro2D::LayerLineScrollBaseRegister, LineScrollAt);
		setLayer(vm, 0, Retro2D::LayerControlRegister, layer.control());
	}

	i64 wrap(i64 value, i64 size) { return ((value % size) + size) % size; }

	// The model: what pixel (x, y) of the screen shows, straight from SPEC 7.5.
	u32 expected(const Layer& layer, u32 x, u32 y)
	{
		i64 dx = 0, dy = 0;
		if (layer.lineScroll)
		{
			dx = static_cast<i16>(layer.shifts[y] & 0xFFFF);
			dy = static_cast<i16>(layer.shifts[y] >> 16);
		}
		const i64 widthPixels = i64{ layer.mapWidth() } * layer.tileSize;
		const i64 heightPixels = i64{ layer.mapHeight() } * layer.tileSize;
		const i64 mx = wrap(i64{ x } + layer.scrollX + dx, widthPixels);
		const i64 my = wrap(i64{ y } + layer.scrollY + dy, heightPixels);
		const u32 entry = layer.map[static_cast<usize>((my / layer.tileSize) * layer.mapWidth() + mx / layer.tileSize)];
		u32 px = static_cast<u32>(mx % layer.tileSize);
		u32 py = static_cast<u32>(my % layer.tileSize);
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
		if (layer.bpp == 8)
			return layer.palette[index];
		return layer.palette[(((entry >> 10) & 7) + layer.bank) % 16 * 16 + index];
	}

	void blank(CeresVM& vm, GpuDevice& gpu)
	{
		Scheduler& events = vm.io().scheduler();
		events.service(events.cycleOf(gpu, GpuDevice::VblankEvent));
	}
}

TEST(tile_layers, every_tile_size_depth_and_map_size_matches_the_model)
{
	struct Case
	{
		u32 tileSize, bpp, mapCode, mapHeightCode, scrollX, scrollY, bank;
		bool lineScroll;
	};
	const Case cases[] = {
		{ 8, 4, 0, 0, 0, 0, 0, false },
		{ 8, 4, 1, 0, 300, 17, 5, false },
		{ 8, 4, 2, 1, 0xFFFFFFF0u, 1000, 15, true },
		{ 8, 4, 3, 2, 12345, 0xFFFFFF00u, 9, false },
		{ 8, 8, 0, 0, 3, 5, 0, false },
		{ 8, 8, 1, 2, 511, 7, 3, true },
		{ 8, 8, 2, 0, 2000, 2000, 0, false },
		{ 8, 8, 0, 1, 0x80000000u, 1, 7, true },
		{ 16, 4, 0, 0, 1, 2, 1, false },
		{ 16, 4, 1, 1, 777, 333, 12, true },
		{ 16, 4, 2, 2, 0xFFFFFFFFu, 0xFFFFFFFFu, 4, false },
		{ 16, 4, 0, 2, 64, 4000, 8, true },
		{ 16, 8, 0, 0, 0, 0, 0, false },
		{ 16, 8, 1, 0, 100, 900, 2, true },
		{ 16, 8, 2, 1, 4095, 17, 0, false },
		{ 16, 8, 2, 2, 31, 2047, 6, true },
	};
	u64 combined = 0;
	u32 seed = 1;
	for (const Case& c : cases)
	{
		CeresVM vm;
		GpuDevice gpu;
		gpu.attachTo(vm.io());
		Layer layer{ .tileSize = c.tileSize, .bpp = c.bpp, .mapCode = c.mapCode, .mapHeightCode = c.mapHeightCode,
			.scrollX = c.scrollX, .scrollY = c.scrollY, .bank = c.bank, .lineScroll = c.lineScroll };
		fill(layer, seed++);
		upload(vm, layer);
		show(vm, layer);
		video::VideoFrame frame;
		gpu.compose(frame);
		u32 wrong = 0;
		for (u32 y = 0; y < ScreenHeight; ++y)
			for (u32 x = 0; x < ScreenWidth; ++x)
				if (frame.pixels[y * ScreenWidth + x] != expected(layer, x, y))
					++wrong;
		CHECK_EQ(wrong, 0u);
		combined = combined * 31 + fnv1a(frame.pixels);
		gpu.detachFrom(vm.io());
	}
	CHECK_EQ(combined, 18169103051324508670ull);   // every case's whole frame, fixed
}

TEST(tile_layers, the_registers_read_back_and_the_gpu_reset_clears_them)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	CHECK_EQ(get(vm, GpuDevice::CapsRegister.value()) & 0x7Fu, 0x7u);   // V0, V1 and V2
	set(vm, GpuDevice::ModeRegister.value(), 2);
	CHECK_EQ(get(vm, GpuDevice::ModeRegister.value()), 2u);
	set(vm, Retro2D::TilePaletteBaseRegister, 0xA0001000);
	setLayer(vm, 3, Retro2D::LayerControlRegister, 0xFFFFFFFFu);
	setLayer(vm, 3, Retro2D::LayerScrollYRegister, 77);
	CHECK_EQ(get(vm, Retro2D::TilePaletteBaseRegister), 0xA0001000u);
	CHECK_EQ(get(vm, Retro2D::layerRegister(3, Retro2D::LayerControlRegister)), 0xFFFFu);   // 16 bits of control
	CHECK_EQ(get(vm, Retro2D::layerRegister(3, Retro2D::LayerScrollYRegister)), 77u);
	CHECK_EQ(get(vm, Retro2D::layerRegister(3, 0x18)), 0u);   // reserved
	set(vm, GpuDevice::ControlRegister.value(), GpuDevice::ControlReset);
	CHECK_EQ(get(vm, Retro2D::TilePaletteBaseRegister), 0u);
	CHECK_EQ(get(vm, Retro2D::layerRegister(3, Retro2D::LayerControlRegister)), 0u);
	gpu.detachFrom(vm.io());
}

TEST(tile_layers, priorities_order_the_layers_and_the_tiles)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	Vram& vram = vm.vram();
	// Tile 1: every pixel colour 1; palette bank n colour 1 is 0x0000n0 + 1 (so the bank shows in the colour).
	for (u32 i = 0; i < 32; ++i)
		vram.write<u8>(TilesAt - Vram::BaseValue + 32 + i, 0x11);
	for (u32 bank = 0; bank < 16; ++bank)
		vram.write<u32>(PaletteAt - Vram::BaseValue + 4 * (bank * 16 + 1), bank * 0x10 + 1);
	// Four maps of 32x32 entries: map n at MapAt + n * 0x800, every entry tile 1 with palette n, and in map 1 the
	// first tile of the row with the priority bit.
	for (u32 n = 0; n < 4; ++n)
		for (u32 i = 0; i < 32 * 32; ++i)
		{
			u32 entry = 1u | (n << MapEntry::PaletteShift);
			if (n == 1 && i % 32 == 0)
				entry |= MapEntry::Priority;
			vram.write<u16>(MapAt - Vram::BaseValue + n * 0x800 + 2 * i, static_cast<u16>(entry));
		}
	Layer none;
	show(vm, none);
	setLayer(vm, 0, Retro2D::LayerControlRegister, 0);
	for (u32 n = 0; n < 4; ++n)
	{
		setLayer(vm, n, Retro2D::LayerMapBaseRegister, MapAt + n * 0x800);
		setLayer(vm, n, Retro2D::LayerTileBaseRegister, TilesAt);
	}
	const auto layerOn = [&](u32 n, u32 priority)
	{
		setLayer(vm, n, Retro2D::LayerControlRegister, LayerControl::Enable | (priority << LayerControl::PriorityShift));
	};
	video::VideoFrame frame;

	// The same priority: the lower number is in front.
	layerOn(2, 1);
	layerOn(3, 1);
	gpu.compose(frame);
	CHECK_EQ(frame.pixels[20], 0x21u);
	// A higher priority is in front, whatever its number.
	layerOn(3, 2);
	gpu.compose(frame);
	CHECK_EQ(frame.pixels[20], 0x31u);
	// Layer 1 at priority 1 is behind layer 3, but the first tile of each of its rows has the priority bit: there it
	// is at 2, ties with layer 3, and the lower number wins.
	layerOn(1, 1);
	gpu.compose(frame);
	CHECK_EQ(frame.pixels[0], 0x11u);
	CHECK_EQ(frame.pixels[8], 0x31u);
	// Mode 1 hides the tile layers.
	set(vm, GpuDevice::ModeRegister.value(), 1);
	gpu.compose(frame);
	CHECK_EQ(frame.pixels[0], Background);
	gpu.detachFrom(vm.io());
}

TEST(tile_layers, a_transparent_pixel_shows_the_bitmap_plane_behind)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	Vram& vram = vm.vram();
	// Tile 0 all transparent, tile 1 colour 2 in its left half.
	for (u32 row = 0; row < 8; ++row)
		for (u32 b = 0; b < 2; ++b)
			vram.write<u8>(TilesAt - Vram::BaseValue + 32 + row * 4 + b, 0x22);
	vram.write<u32>(PaletteAt - Vram::BaseValue + 8, 0xABCDEF);
	vram.write<u16>(MapAt - Vram::BaseValue, 1);
	Layer none;
	show(vm, none);
	setLayer(vm, 0, Retro2D::LayerControlRegister, LayerControl::Enable);
	// The bitmap plane: one I8 row of colour 5 from the text plane's palette, repeated over the screen.
	const u32 picture = 0xA0180000;
	for (u32 x = 0; x < ScreenWidth; ++x)
		vram.write<u8>(picture - Vram::BaseValue + x, 5);
	set(vm, video::BitmapPlane::EnableRegister, 1);
	set(vm, video::BitmapPlane::FormatRegister, 3);
	set(vm, video::BitmapPlane::WidthRegister, ScreenWidth);
	set(vm, video::BitmapPlane::HeightRegister, 1);
	set(vm, video::BitmapPlane::PitchRegister, ScreenWidth);
	set(vm, video::BitmapPlane::BaseRegister, picture);
	set(vm, GpuDevice::PresentRegister.value(), 1);
	blank(vm, gpu);
	video::VideoFrame frame;
	gpu.compose(frame);
	const u32 bitmapColour = vram.read<u32>(get(vm, video::BitmapPlane::PaletteBaseRegister) - Vram::BaseValue + 4 * 5) & 0xFFFFFF;
	CHECK_EQ(frame.pixels[0], 0xABCDEFu);
	CHECK_EQ(frame.pixels[3], 0xABCDEFu);
	CHECK_EQ(frame.pixels[4], bitmapColour);    // the tile's right half is transparent
	CHECK_EQ(frame.pixels[8], bitmapColour);    // tile 0
	gpu.detachFrom(vm.io());
}

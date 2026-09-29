#include "device_test_machine.h"

#include <ceres/devices/video/gpu.h>

// The sprites of V2 (plan/v2 SPEC 7.5): 128 entries in VRAM of every size, flip, depth and priority, over and under a
// tile layer, compared pixel by pixel with a plain model written here; the limit of sprites a line and its status;
// and, on micro and pocket, the VRAM the CPU may only write in the vertical blank (D17).

namespace
{
	using video::LayerControl;
	using video::OamEntry;
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

	constexpr u32 TilePaletteAt = 0xA0100000;
	constexpr u32 SpritePaletteAt = 0xA0100400;
	constexpr u32 OamAt = 0xA0101000;
	constexpr u32 MapAt = 0xA0110000;
	constexpr u32 TilesAt = 0xA0120000;
	constexpr u32 PicturesAt = 0xA0130000;
	constexpr u32 ScreenWidth = 96;
	constexpr u32 ScreenHeight = 64;
	constexpr u32 Background = 0x0A0B0C;
	constexpr u32 LayerColour = 0x445566;

	struct Lcg
	{
		u32 state;
		u32 next() { state = state * 1664525u + 1013904223u; return state >> 8; }
	};

	// What the test put in the OAM, the pictures and the palette, for the model to read back.
	struct Scene
	{
		std::vector<u32> oam;        // two words a sprite
		std::vector<u8> pictures;
		std::vector<u32> palette;
		u32 layerPriority = 0;
		u32 limit = 128;
	};

	Scene makeScene(u32 seed, u32 layerPriority)
	{
		Lcg random{ seed };
		Scene scene;
		scene.layerPriority = layerPriority;
		scene.pictures.resize(0x10000);
		for (u8& byte : scene.pictures)
			byte = static_cast<u8>(random.next() % 3 == 0 ? 0 : random.next());
		scene.palette.resize(256);
		for (u32& colour : scene.palette)
			colour = random.next() & 0x00FFFFFFu;
		for (u32 i = 0; i < OamEntry::Count; ++i)
		{
			const i32 x = static_cast<i32>(random.next() % (ScreenWidth + 80)) - 64;
			const i32 y = static_cast<i32>(random.next() % (ScreenHeight + 80)) - 64;
			u32 attributes = (random.next() % 1500) | (random.next() & 0x3FFF0000u);   // graphic, bank, size, flips, priority, depth
			if (random.next() % 5 != 0)
				attributes |= OamEntry::Visible;
			scene.oam.push_back((static_cast<u32>(x) & 0xFFFF) | (static_cast<u32>(y) << 16));
			scene.oam.push_back(attributes);
		}
		return scene;
	}

	// The layer behind: tile 1 everywhere, whose left half is colour 1 and right half transparent.
	void show(CeresVM& vm, const Scene& scene)
	{
		Vram& vram = vm.vram();
		for (u32 row = 0; row < 8; ++row)
			for (u32 b = 0; b < 2; ++b)
				vram.write<u8>(TilesAt - Vram::BaseValue + 32 + row * 4 + b, 0x11);
		vram.write<u32>(TilePaletteAt - Vram::BaseValue + 4, LayerColour);
		for (u32 i = 0; i < 32 * 32; ++i)
			vram.write<u16>(MapAt - Vram::BaseValue + 2 * i, 1);
		for (usize i = 0; i < scene.pictures.size(); ++i)
			vram.write<u8>(PicturesAt - Vram::BaseValue + static_cast<u32>(i), scene.pictures[i]);
		for (usize i = 0; i < scene.palette.size(); ++i)
			vram.write<u32>(SpritePaletteAt - Vram::BaseValue + static_cast<u32>(4 * i), scene.palette[i]);
		for (usize i = 0; i < OamEntry::Count; ++i)
		{
			vram.write<u32>(OamAt - Vram::BaseValue + static_cast<u32>(16 * i), scene.oam[2 * i]);
			vram.write<u32>(OamAt - Vram::BaseValue + static_cast<u32>(16 * i + 4), scene.oam[2 * i + 1]);
		}

		set(vm, GpuDevice::WidthRegister.value(), ScreenWidth);
		set(vm, GpuDevice::HeightRegister.value(), ScreenHeight);
		set(vm, GpuDevice::ModeRegister.value(), 2);
		set(vm, GpuDevice::BackgroundColorRegister.value(), Background);
		set(vm, video::TextPlane::EnableRegister, 0);
		set(vm, Retro2D::TilePaletteBaseRegister, TilePaletteAt);
		set(vm, Retro2D::SpritePaletteBaseRegister, SpritePaletteAt);
		set(vm, Retro2D::OamBaseRegister, OamAt);
		set(vm, Retro2D::SpriteTileBaseRegister, PicturesAt);
		set(vm, Retro2D::SpriteControlRegister, Retro2D::SpritesOn);
		set(vm, Retro2D::layerRegister(0, Retro2D::LayerMapBaseRegister), MapAt);
		set(vm, Retro2D::layerRegister(0, Retro2D::LayerTileBaseRegister), TilesAt);
		set(vm, Retro2D::layerRegister(0, Retro2D::LayerControlRegister), LayerControl::Enable | (scene.layerPriority << LayerControl::PriorityShift));
	}

	// The model, from SPEC 7.5: the sprite pixel is the first opaque one among the line's sprites in OAM order (the
	// first `limit` visible ones that touch the line), and it goes in front of the layer when its priority is not lower.
	u32 expected(const Scene& scene, u32 x, u32 y)
	{
		const bool layerHere = (x % 8) < 4;
		u32 taken = 0;
		for (u32 i = 0; i < OamEntry::Count; ++i)
		{
			const u32 position = scene.oam[2 * i];
			const u32 attributes = scene.oam[2 * i + 1];
			if ((attributes & OamEntry::Visible) == 0)
				continue;
			const i32 sx = static_cast<i16>(position & 0xFFFF);
			const i32 sy = static_cast<i16>(position >> 16);
			const i32 width = 8 << ((attributes >> 20) & 3);
			const i32 height = 8 << ((attributes >> 22) & 3);
			if (static_cast<i32>(y) < sy || static_cast<i32>(y) >= sy + height)
				continue;
			if (++taken > scene.limit)
				break;
			if (static_cast<i32>(x) < sx || static_cast<i32>(x) >= sx + width)
				continue;
			i32 px = static_cast<i32>(x) - sx;
			i32 py = static_cast<i32>(y) - sy;
			if (attributes & OamEntry::FlipX)
				px = width - 1 - px;
			if (attributes & OamEntry::FlipY)
				py = height - 1 - py;
			const bool deep = (attributes & OamEntry::Bpp8) != 0;
			const usize base = (attributes & 0xFFFF) * 32;
			const usize pixel = static_cast<usize>(py * width + px);
			const u32 index = deep ? scene.pictures[base + pixel] : (pixel % 2 == 0 ? scene.pictures[base + pixel / 2] >> 4 : scene.pictures[base + pixel / 2] & 15);
			if (index == 0)
				continue;
			const u32 priority = (attributes >> 26) & 3;
			if (layerHere && priority < scene.layerPriority)
				return LayerColour;
			return scene.palette[deep ? index : ((attributes >> 16) & 15) * 16 + index];
		}
		return layerHere ? LayerColour : Background;
	}

	void blank(CeresVM& vm, GpuDevice& gpu)
	{
		Scheduler& events = vm.io().scheduler();
		events.service(events.cycleOf(gpu, GpuDevice::VblankEvent));
	}

	// OAM entry i: a visible 8x8 sprite at (x, y) whose picture is graphic 0.
	void placeSprite(CeresVM& vm, u32 i, i32 x, i32 y)
	{
		vm.vram().write<u32>(OamAt - Vram::BaseValue + 16 * i, (static_cast<u32>(x) & 0xFFFF) | (static_cast<u32>(y) << 16));
		vm.vram().write<u32>(OamAt - Vram::BaseValue + 16 * i + 4, OamEntry::Visible);
	}
}

TEST(sprites, every_size_flip_depth_and_priority_matches_the_model)
{
	u64 combined = 0;
	for (u32 seed = 1; seed <= 8; ++seed)
	{
		CeresVM vm;
		GpuDevice gpu;
		gpu.attachTo(vm.io());
		const Scene scene = makeScene(seed, seed % 4);
		show(vm, scene);
		video::VideoFrame frame;
		gpu.compose(frame);
		u32 wrong = 0;
		u32 sprites = 0;
		for (u32 y = 0; y < ScreenHeight; ++y)
			for (u32 x = 0; x < ScreenWidth; ++x)
			{
				const u32 model = expected(scene, x, y);
				if (frame.pixels[y * ScreenWidth + x] != model)
					++wrong;
				if (model != Background && model != LayerColour)
					++sprites;
			}
		CHECK_EQ(wrong, 0u);
		CHECK(sprites > ScreenWidth * ScreenHeight / 4);
		combined = combined * 31 + fnv1a(frame.pixels);
		gpu.detachFrom(vm.io());
	}
	CHECK_EQ(combined, 5061644992028013290ull);   // every scene's whole frame, fixed
}

TEST(sprites, a_line_shows_no_more_than_the_limit_and_the_status_says_so)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.configure(GpuDevice::Config{ .gpuClockHz = 2'000'000, .maxLevel = 2, .maxWidth = 256, .maxHeight = 192, .refresh = 60, .spritesPerLine = 16 });
	gpu.attachTo(vm.io());
	CHECK_EQ(get(vm, Retro2D::SpriteLimitRegister), 16u);
	Scene none = makeScene(1, 0);
	show(vm, none);
	set(vm, Retro2D::layerRegister(0, Retro2D::LayerControlRegister), 0);
	// Graphic 0: every pixel colour 1 (4 bpp), bank 0 colour 1 white.
	for (u32 i = 0; i < 32; ++i)
		vm.vram().write<u8>(PicturesAt - Vram::BaseValue + i, 0x11);
	vm.vram().write<u32>(SpritePaletteAt - Vram::BaseValue + 4, 0xFFFFFF);
	for (u32 i = 0; i < OamEntry::Count; ++i)
		vm.vram().write<u32>(OamAt - Vram::BaseValue + 16 * i + 4, 0);   // none visible
	// Twenty sprites side by side on lines 20-27, and one alone on line 40.
	for (u32 i = 0; i < 20; ++i)
		placeSprite(vm, i, static_cast<i32>(i * 4), 20);
	placeSprite(vm, 20, 0, 40);

	video::VideoFrame frame;
	gpu.compose(frame);
	// The first sixteen are drawn (they overlap, so 4 * 15 + 8 pixels); the other four are not.
	u32 lit = 0;
	for (u32 x = 0; x < ScreenWidth; ++x)
		lit += frame.pixels[20 * ScreenWidth + x] == 0xFFFFFFu ? 1 : 0;
	CHECK_EQ(lit, 68u);
	CHECK_EQ(frame.pixels[40 * ScreenWidth + 3], 0xFFFFFFu);

	CHECK_EQ(get(vm, Retro2D::SpriteStatusRegister), 0u);   // nothing counted before a vertical blank
	blank(vm, gpu);
	CHECK_EQ(get(vm, Retro2D::SpriteStatusRegister), Retro2D::SpriteOverflow | (20u << Retro2D::SpriteOverflowLineShift));
	// Down to sixteen, the next frame has no overflow.
	vm.vram().write<u32>(OamAt - Vram::BaseValue + 16 * 3 + 4, 0);
	vm.vram().write<u32>(OamAt - Vram::BaseValue + 16 * 7 + 4, 0);
	vm.vram().write<u32>(OamAt - Vram::BaseValue + 16 * 11 + 4, 0);
	vm.vram().write<u32>(OamAt - Vram::BaseValue + 16 * 12 + 4, 0);
	blank(vm, gpu);
	CHECK_EQ(get(vm, Retro2D::SpriteStatusRegister), 0u);
	// Nor with the sprites off.
	placeSprite(vm, 3, 0, 20);
	set(vm, Retro2D::SpriteControlRegister, 0);
	blank(vm, gpu);
	CHECK_EQ(get(vm, Retro2D::SpriteStatusRegister), 0u);
	gpu.detachFrom(vm.io());
}

TEST(sprites, sprites_off_the_screen_edges_are_clipped_and_counted)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.configure(GpuDevice::Config{ .gpuClockHz = 2'000'000, .maxLevel = 2, .maxWidth = 256, .maxHeight = 192, .refresh = 60, .spritesPerLine = 1 });
	gpu.attachTo(vm.io());
	Scene none = makeScene(1, 0);
	show(vm, none);
	set(vm, Retro2D::layerRegister(0, Retro2D::LayerControlRegister), 0);
	for (u32 i = 0; i < 32; ++i)
		vm.vram().write<u8>(PicturesAt - Vram::BaseValue + i, 0x11);
	vm.vram().write<u32>(SpritePaletteAt - Vram::BaseValue + 4, 0xFFFFFF);
	for (u32 i = 0; i < OamEntry::Count; ++i)
		vm.vram().write<u32>(OamAt - Vram::BaseValue + 16 * i + 4, 0);
	// Sprite 0 hangs off the left edge by 5, so 3 columns show; it still takes the line's one place, and sprite 1,
	// further along, is left out.
	placeSprite(vm, 0, -5, -3);
	placeSprite(vm, 1, 20, 0);
	video::VideoFrame frame;
	gpu.compose(frame);
	CHECK_EQ(frame.pixels[0], 0xFFFFFFu);
	CHECK_EQ(frame.pixels[2], 0xFFFFFFu);
	CHECK_EQ(frame.pixels[3], Background);
	CHECK_EQ(frame.pixels[4 * ScreenWidth], 0xFFFFFFu);   // its last row is line 4
	CHECK_EQ(frame.pixels[5 * ScreenWidth], Background);
	CHECK_EQ(frame.pixels[20], Background);
	CHECK_EQ(frame.pixels[5 * ScreenWidth + 20], 0xFFFFFFu);   // below sprite 0, sprite 1 has the line to itself
	blank(vm, gpu);
	CHECK_EQ(get(vm, Retro2D::SpriteStatusRegister), Retro2D::SpriteOverflow);   // line 0
	gpu.detachFrom(vm.io());
}

TEST(sprites, on_micro_the_cpu_writes_the_vram_only_in_the_vertical_blank)
{
	constexpr u32 Target = 0xA0004000;
	const GpuDevice::Config micro{ .gpuClockHz = 2'000'000, .maxLevel = 2, .maxWidth = 256, .maxHeight = 192, .refresh = 60,
		.spritesPerLine = 16, .vramInVblankOnly = true };
	Machine m{ Instruction::STR(1, 2, 0) };
	CeresVM& vm = m.vm();
	GpuDevice gpu;
	gpu.configure(micro);
	gpu.attachTo(vm.io());
	CHECK(vm.vram().writeGate() == &gpu);
	vm.engine().setRegister(1, Target);
	vm.engine().setRegister(2, 0x12345678);
	set(vm, GpuDevice::IrqEnableRegister.value(), GpuDevice::IrqFault);

	// Line 0: the store is dropped and the GPU faults, without the CPU faulting.
	m.step();
	CHECK_EQ(vm.vram().read<u32>(Target - Vram::BaseValue), 0u);
	CHECK_EQ(get(vm, GpuDevice::FaultCodeRegister.value()), GpuDevice::FaultVramBusy);
	CHECK_EQ(get(vm, GpuDevice::FaultAddressRegister.value()), Target);
	CHECK(vm.interrupts().peek() == GpuDevice::FaultInterrupt);
	CHECK_EQ(m.pc().value(), EntryPoint + 4);

	// With the display off it goes in.
	set(vm, GpuDevice::ControlRegister.value(), 0);
	CHECK(gpu.admitCpuStore(Target));
	set(vm, GpuDevice::ControlRegister.value(), GpuDevice::ControlDisplayOn);
	CHECK(!gpu.admitCpuStore(Target));
	gpu.detachFrom(vm.io());
	CHECK(vm.vram().writeGate() == nullptr);

	// And in the vertical blank: a halted machine wakes at its first line.
	Machine halted{ Instruction::HALT() };
	GpuDevice other;
	other.configure(micro);
	other.attachTo(halted.vm().io());
	halted.step();   // the HALT
	halted.step();   // up to the vertical blank
	CHECK((get(halted.vm(), GpuDevice::StatusRegister.value()) & GpuDevice::StatusVblank) != 0);
	CHECK(other.admitCpuStore(Target));
	other.detachFrom(halted.vm().io());
}

TEST(sprites, the_other_profiles_leave_the_vram_open)
{
	CeresVM vm;
	GpuDevice gpu;
	gpu.attachTo(vm.io());
	CHECK(vm.vram().writeGate() == nullptr);
	CHECK_EQ(get(vm, Retro2D::SpriteLimitRegister), 128u);
	gpu.detachFrom(vm.io());
}

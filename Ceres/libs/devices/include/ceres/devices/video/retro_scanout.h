#pragma once

// One line of the Retro 2D level, V2, in software (plan/v2 SPEC 7.5): the tile layers over what the line already
// holds (the background colour and the bitmap plane), in the order of their priorities. The software executor calls
// it for every line of a frame in V2; what it draws is what every other executor has to match.

#include <ceres/devices/video/retro2d.h>
#include <ceres/vm/vram.h>

#include <array>
#include <span>
#include <vector>

namespace ceres::devices::video
{
	class RetroScanout
	{
	public:
		// What a layer's pixel is drawn at when it has nothing to draw there.
		static inline constexpr u8 Transparent = 0xFF;
		// The layers in the order they are drawn among the same priority, from the back (SPEC 7.5): 3, 2, 1, 0.
		static inline constexpr u32 LayerSlots = Retro2D::LayerCount;

	private:
		// A layer's pixels on the line being drawn: the colour, and the priority it is drawn at (or Transparent).
		struct LayerLine
		{
			std::vector<u32> colours;
			std::vector<u8> priorities;
		};

		// A palette as the VRAM held it when it was read, kept for the rest of the frame while its base stays.
		struct Palette
		{
			u32 base = 0;
			bool loaded = false;
			std::array<u32, Retro2D::PaletteEntries> colours{};
		};

		std::array<LayerLine, LayerSlots> _layers;
		Palette _tilePalette;

	public:
		// A new frame: the palettes are read from the VRAM again.
		void beginFrame() noexcept;
		// Draws line `y` of V2 over `line` (0x00RRGGBB), which holds what is behind it.
		void composeLine(const Retro2D& retro, const vm::Vram& vram, u32 y, std::span<u32> line);

	private:
		const Palette& palette(Palette& cache, u32 base, const vm::Vram& vram);
		void drawTileLayer(const TileLayer& layer, const Palette& palette, const vm::Vram& vram, u32 y, LayerLine& out);
	};
}

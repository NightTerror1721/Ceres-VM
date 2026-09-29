#pragma once

// The GPU's bitmap plane (plan/v2 SPEC 7.1 V1, 7.3 0x240-0x27F): a picture in VRAM behind the text plane, in one of
// the eight formats of formats.h, Pitch bytes from a row to the next. ScrollX and ScrollY move the picture under the
// screen, wrapping round, so a plane larger than the screen scrolls for free and a smaller one repeats. The indexed
// formats look their colours up in a palette of 256 entries of 0x00RRGGBB at PaletteBase; the formats with alpha are
// blended over the background colour.
//
// Buffers says how many pictures take turns (1-3), and a Present (the GPU's register) flips them at the vertical
// blank, so what is shown is never what is being drawn:
//   1  nothing flips: the program draws on what is shown.
//   2  Base and BackBase swap at the vertical blank; draw into BackBase once the flip is done (Status bit 3 clear).
//   3  at the Present, BackBase becomes the picture waiting to be shown and SpareBase the new BackBase, so the program
//      can go on drawing at once; at the vertical blank the waiting one is shown and the one it replaces is the new
//      SpareBase. A second Present before the blank swaps the waiting picture with BackBase.
// A write to Base or PaletteBase, like the text plane's bases, takes effect at the vertical blank after a Present.

#include <ceres/core/base/types.h>
#include <ceres/vm/vram.h>

namespace ceres::devices::video
{
	class BitmapPlane
	{
	public:
		static inline constexpr u32 EnableRegister = 0x240;       // RW: 1 shows the plane (in Mode 1 and above)
		static inline constexpr u32 BaseRegister = 0x244;         // RW: the picture shown
		static inline constexpr u32 BackBaseRegister = 0x248;     // RW: the picture to draw into
		static inline constexpr u32 PitchRegister = 0x24C;        // RW: bytes from a row to the next
		static inline constexpr u32 FormatRegister = 0x250;       // RW: formats.h, 0-7
		static inline constexpr u32 WidthRegister = 0x254;        // RW: the picture's size in pixels
		static inline constexpr u32 HeightRegister = 0x258;       // RW
		static inline constexpr u32 ScrollXRegister = 0x25C;      // RW: the picture's column at the screen's left edge
		static inline constexpr u32 ScrollYRegister = 0x260;      // RW: its row at the top
		static inline constexpr u32 BuffersRegister = 0x264;      // RW: 1-3
		static inline constexpr u32 PaletteBaseRegister = 0x268;  // RW: 256 entries of 0x00RRGGBB for I1-I8
		static inline constexpr u32 SpareBaseRegister = 0x26C;    // RW: the third picture, with Buffers 3
		static inline constexpr u32 FirstRegister = 0x240;
		static inline constexpr u32 LastRegister = 0x27C;

	private:
		bool _enabled = false;
		u32 _base = 0, _backBase = 0, _spareBase = 0;
		u32 _pitch = 0;
		u32 _format = 6;   // XRGB8888
		u32 _width = 0, _height = 0;
		u32 _scrollX = 0, _scrollY = 0;
		u32 _buffers = 1;
		u32 _paletteBase = 0;
		// What the scanout uses: the bases as of the last vertical blank that followed a Present.
		u32 _shownBase = 0, _shownPalette = 0;
		// With three buffers, the picture a Present queued for the next vertical blank.
		bool _queued = false;
		u32 _queuedBase = 0;

	public:
		// Back to the start: off, the screen's size in XRGB8888, one picture at `base` (the next two after it, when
		// they fit), the palette at `paletteBase`.
		void reset(u32 width, u32 height, u32 base, u32 paletteBase, usize vramSize) noexcept;

		static constexpr bool handles(u32 offset) noexcept { return offset >= FirstRegister && offset <= LastRegister; }
		u32 read(u32 offset) const noexcept;
		void write(u32 offset, u32 value) noexcept;

		// The program wrote the GPU's Present: a triple-buffered plane queues its back buffer now.
		void present() noexcept;
		// A Present came to its vertical blank: flip, and apply the bases written.
		void applyPending() noexcept;

		bool enabled() const noexcept { return _enabled; }
		u32 shownBase() const noexcept { return _shownBase; }
		u32 shownPalette() const noexcept { return _shownPalette; }
		u32 pitch() const noexcept { return _pitch; }
		u32 format() const noexcept { return _format; }
		u32 width() const noexcept { return _width; }
		u32 height() const noexcept { return _height; }
		u32 scrollX() const noexcept { return _scrollX; }
		u32 scrollY() const noexcept { return _scrollY; }
		u32 buffers() const noexcept { return _buffers; }
		u32 base() const noexcept { return _base; }
		u32 backBase() const noexcept { return _backBase; }
		u32 spareBase() const noexcept { return _spareBase; }

		bool operator==(const BitmapPlane&) const noexcept = default;
	};
}

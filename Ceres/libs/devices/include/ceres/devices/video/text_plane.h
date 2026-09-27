#pragma once

// The GPU's text plane (plan/v2 SPEC 7.1 V0, 7.3 0x200-0x23F, 7.4): a grid of 8x16 cells drawn over everything else,
// its cells, font and palette all in VRAM. Part of the GPU, not a device of its own: the GPU hands it the offsets of
// its block and the scanout reads it.
//
// A cell is 16 bits by default: bits 7:0 the glyph, 11:8 the ink and 15:12 the background, indices into the palette's
// first 16 entries. The 32-bit cell (CellFormat 1) has the glyph in 7:0, the ink in 15:8 and the background in 23:16,
// any of the 256 entries, and bit 24 underlines, bit 25 swaps ink and background. A background of 0 is transparent:
// what is below (the bitmap plane, the background colour) shows through, so a terminal over a black background looks
// the same and over a picture writes on it.
//
// The rows above the screen that scrolled away live in the scrollback, a ring of ScrollbackLines rows of Cols cells
// at ScrollbackBase: ScrollbackHead is the row the next one goes in and ScrollbackCount how many it holds. ScrollY > 0
// shows that many of them at the top, the screen pushed down (the terminal's Shift+PageUp).

#include <ceres/core/base/types.h>
#include <ceres/vm/vram.h>

#include <string>

namespace ceres::devices::video
{
	class TextPlane
	{
	public:
		// Offsets in the GPU's slot.
		static inline constexpr u32 EnableRegister = 0x200;          // RW: 1 shows the plane
		static inline constexpr u32 ColsRegister = 0x204;            // R: Width / 8
		static inline constexpr u32 RowsRegister = 0x208;            // R: Height / 16
		static inline constexpr u32 CellsBaseRegister = 0x20C;       // RW: the first cell (a Present applies it)
		static inline constexpr u32 CellFormatRegister = 0x210;      // RW: 0 16-bit cells, 1 32-bit
		static inline constexpr u32 FontBaseRegister = 0x214;        // RW: 16 bytes a glyph (a Present applies it)
		static inline constexpr u32 GlyphCountRegister = 0x218;      // RW: glyphs in the font, 1-256; others draw blank
		static inline constexpr u32 PaletteBaseRegister = 0x21C;     // RW: 256 entries of 0x00RRGGBB (a Present applies it)
		static inline constexpr u32 CursorXRegister = 0x220;         // RW: the cursor's column
		static inline constexpr u32 CursorYRegister = 0x224;         // RW: its row
		static inline constexpr u32 CursorShapeRegister = 0x228;     // RW: bits 1:0 none, underline, block, bar; bit 8 blinks
		static inline constexpr u32 ScrollbackBaseRegister = 0x22C;  // RW: the ring of rows that scrolled away
		static inline constexpr u32 ScrollbackLinesRegister = 0x230; // RW: how many rows the ring holds
		static inline constexpr u32 ScrollYRegister = 0x234;         // RW: rows of the ring shown at the top (0: the live screen)
		static inline constexpr u32 ScrollbackHeadRegister = 0x238;  // RW: the row the next scrolled-away line goes in
		static inline constexpr u32 ScrollbackCountRegister = 0x23C; // RW: the rows the ring holds now
		static inline constexpr u32 FirstRegister = 0x200;
		static inline constexpr u32 LastRegister = 0x23C;

		static inline constexpr u32 CellWidth = 8;
		static inline constexpr u32 CellHeight = 16;

		static inline constexpr u32 CursorNone = 0;
		static inline constexpr u32 CursorUnderline = 1;
		static inline constexpr u32 CursorBlock = 2;
		static inline constexpr u32 CursorBar = 3;
		static inline constexpr u32 CursorBlink = 1u << 8;
		// A blinking cursor is shown for this many frames and hidden for as many.
		static inline constexpr u32 BlinkFrames = 16;

		static inline constexpr u32 DefaultInk = 7;
		static inline constexpr u32 DefaultBackground = 0;
		// A blank 16-bit cell: a space, light grey on the transparent background.
		static inline constexpr u16 BlankCell = static_cast<u16>(' ' | (DefaultInk << 8) | (DefaultBackground << 12));

		// Where the GPU puts things in VRAM when it starts (SPEC 7.4): packed from its first byte, each at a multiple
		// of 256 - the cells for the largest terminal of the profile, the font, the palette and the scrollback (a
		// quarter of the VRAM at most, and never more than 256 KiB).
		struct Layout
		{
			u32 cellsBase = 0;
			u32 fontBase = 0;
			u32 paletteBase = 0;
			u32 scrollbackBase = 0;
			u32 scrollbackBytes = 0;
		};
		static Layout bootLayout(u32 maxCols, u32 maxRows, usize vramSize) noexcept;

		// The palette the GPU starts with: the 16 ANSI colours (the classic VGA ones), then xterm's 6x6x6 cube and
		// its 24 greys, so `ESC[38;5;n m` means what it does on a terminal.
		static u32 defaultPaletteEntry(u32 index) noexcept;

	private:
		Layout _layout;
		bool _enabled = true;
		u32 _cols = 80;
		u32 _rows = 30;
		u32 _cellFormat = 0;
		u32 _glyphCount = 256;
		u32 _cursorX = 0;
		u32 _cursorY = 0;
		u32 _cursorShape = CursorNone;
		u32 _scrollbackBase = 0;
		u32 _scrollbackLines = 0;
		u32 _scrollY = 0;
		u32 _scrollbackHead = 0;
		u32 _scrollbackCount = 0;
		// The bases as written, and as the scanout uses them: a Present copies the one to the other at a vertical blank.
		u32 _cellsBase = 0, _fontBase = 0, _paletteBase = 0;
		u32 _cellsBasePending = 0, _fontBasePending = 0, _paletteBasePending = 0;

	public:
		// Back to the start: the layout's bases, the geometry, the cursor hidden and the scrollback empty.
		void reset(const Layout& layout, u32 cols, u32 rows) noexcept;
		// Writes what the plane starts with into VRAM: the font, the palette and blank cells.
		void writeBootData(vm::Vram& vram) const noexcept;
		// The screen changed size: the grid follows, the scrollback empties (its rows are the old width).
		void setGeometry(u32 cols, u32 rows) noexcept;

		static constexpr bool handles(u32 offset) noexcept { return offset >= FirstRegister && offset <= LastRegister; }
		u32 read(u32 offset) const noexcept;
		void write(u32 offset, u32 value) noexcept;
		// A Present came to its vertical blank.
		void applyPending() noexcept;

		bool enabled() const noexcept { return _enabled; }
		u32 cols() const noexcept { return _cols; }
		u32 rows() const noexcept { return _rows; }
		u32 cellsBase() const noexcept { return _cellsBase; }
		u32 cellBytes() const noexcept { return _cellFormat == 1 ? 4u : 2u; }
		u32 cellFormat() const noexcept { return _cellFormat; }
		u32 fontBase() const noexcept { return _fontBase; }
		u32 glyphCount() const noexcept { return _glyphCount; }
		u32 paletteBase() const noexcept { return _paletteBase; }
		u32 cursorX() const noexcept { return _cursorX; }
		u32 cursorY() const noexcept { return _cursorY; }
		u32 cursorShape() const noexcept { return _cursorShape; }
		u32 scrollbackBase() const noexcept { return _scrollbackBase; }
		u32 scrollbackLines() const noexcept { return _scrollbackLines; }
		u32 scrollY() const noexcept { return _scrollY; }
		u32 scrollbackHead() const noexcept { return _scrollbackHead; }
		u32 scrollbackCount() const noexcept { return _scrollbackCount; }
		const Layout& layout() const noexcept { return _layout; }

		// For the terminal, which drives the plane from the host side (plan/v2 SPEC 8).
		void setCursor(u32 x, u32 y, u32 shape) noexcept { _cursorX = x; _cursorY = y; _cursorShape = shape; }
		void setScrollY(u32 rows) noexcept { _scrollY = rows < _scrollbackCount ? rows : _scrollbackCount; }
		void setScrollback(u32 head, u32 count) noexcept { _scrollbackHead = head; _scrollbackCount = count; if (_scrollY > count) _scrollY = count; }

		// The address of the cell at (col, row) of the live screen, and of row `line` of the scrollback ring.
		u32 cellAddress(u32 col, u32 row) const noexcept { return _cellsBase + (row * _cols + col) * cellBytes(); }
		u32 scrollbackRowAddress(u32 line) const noexcept { return _scrollbackBase + line * _cols * cellBytes(); }

		// The cell as it would be drawn: from the live screen, or from the scrollback when ScrollY shows it there.
		// 0 when it is outside the VRAM.
		u32 shownCell(const vm::Vram& vram, u32 col, u32 row) const noexcept;

		// The live screen as text: a line a row, UTF-8, trailing spaces dropped (what --screen-log writes).
		std::string toText(const vm::Vram& vram) const;
	};

	// The code point a glyph stands for: Latin-1, the box characters at 0x80-0x9F, a space for a control.
	u32 glyphCodePoint(u32 glyph) noexcept;
	// The glyph for a code point, or '?' when the font has none (SPEC 8.2).
	u32 glyphFor(u32 codePoint) noexcept;
}

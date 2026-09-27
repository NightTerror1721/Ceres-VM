#include <ceres/devices/video/text_plane.h>
#include <ceres/devices/video/default_font.h>

#include <algorithm>

namespace ceres::devices::video
{
	namespace
	{
		constexpr u32 align256(u64 value) noexcept { return static_cast<u32>((value + 255) & ~u64{ 255 }); }

		constexpr u32 PaletteEntries = 256;

		void appendUtf8(std::string& out, u32 cp)
		{
			if (cp < 0x80)
				out += static_cast<char>(cp);
			else if (cp < 0x800)
			{
				out += static_cast<char>(0xC0 | (cp >> 6));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
			else
			{
				out += static_cast<char>(0xE0 | (cp >> 12));
				out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
		}
	}

	TextPlane::Layout TextPlane::bootLayout(u32 maxCols, u32 maxRows, usize vramSize) noexcept
	{
		Layout layout;
		const u32 base = vm::Vram::BaseValue;
		layout.cellsBase = base;
		layout.fontBase = base + align256(u64{ maxCols } * maxRows * 2);
		layout.paletteBase = layout.fontBase + align256(FontGlyphCount * FontGlyphBytes);
		layout.scrollbackBase = layout.paletteBase + align256(PaletteEntries * 4);
		const u64 used = layout.scrollbackBase - base;
		const u64 quarter = vramSize / 4;
		const u64 room = vramSize > used ? vramSize - used : 0;
		layout.scrollbackBytes = static_cast<u32>(std::min<u64>({ quarter, u64{ 256 * 1024 }, room }));
		return layout;
	}

	u32 TextPlane::defaultPaletteEntry(u32 index) noexcept
	{
		constexpr u32 ansi[16] = {
			0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
			0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF };
		index &= 255;
		if (index < 16)
			return ansi[index];
		if (index < 232)
		{
			constexpr u32 levels[6] = { 0, 95, 135, 175, 215, 255 };
			const u32 i = index - 16;
			return (levels[i / 36] << 16) | (levels[(i / 6) % 6] << 8) | levels[i % 6];
		}
		const u32 grey = 8 + (index - 232) * 10;
		return (grey << 16) | (grey << 8) | grey;
	}

	void TextPlane::reset(const Layout& layout, u32 cols, u32 rows) noexcept
	{
		_layout = layout;
		_enabled = true;
		_cellFormat = 0;
		_glyphCount = FontGlyphCount;
		_cursorX = _cursorY = 0;
		_cursorShape = CursorNone;
		_cellsBase = _cellsBasePending = layout.cellsBase;
		_fontBase = _fontBasePending = layout.fontBase;
		_paletteBase = _paletteBasePending = layout.paletteBase;
		_scrollbackBase = layout.scrollbackBase;
		setGeometry(cols, rows);
	}

	void TextPlane::setGeometry(u32 cols, u32 rows) noexcept
	{
		_cols = std::max<u32>(cols, 1);
		_rows = std::max<u32>(rows, 1);
		_scrollbackLines = _layout.scrollbackBytes / (_cols * cellBytes());
		_scrollbackHead = 0;
		_scrollbackCount = 0;
		_scrollY = 0;
		_cursorX = std::min(_cursorX, _cols - 1);
		_cursorY = std::min(_cursorY, _rows - 1);
	}

	void TextPlane::writeBootData(vm::Vram& vram) const noexcept
	{
		const auto put = [&vram](u32 address, const u8* bytes, u32 count)
		{
			const u32 offset = address - vm::Vram::BaseValue;
			const u32 fits = vram.clamp(offset, count);
			if (fits != 0)
				std::copy(bytes, bytes + fits, vram.span(offset, fits));
		};
		put(_layout.fontBase, DefaultFont8x16.data(), FontGlyphCount * FontGlyphBytes);

		u8 palette[PaletteEntries * 4];
		for (u32 i = 0; i < PaletteEntries; ++i)
		{
			const u32 colour = defaultPaletteEntry(i);
			for (u32 b = 0; b < 4; ++b)
				palette[i * 4 + b] = static_cast<u8>(colour >> (8 * b));
		}
		put(_layout.paletteBase, palette, sizeof(palette));

		// The cells of the largest screen, blank: the font starts where they end.
		const u32 cellBytesTotal = _layout.fontBase - _layout.cellsBase;
		const u32 offset = _layout.cellsBase - vm::Vram::BaseValue;
		const u32 fits = vram.clamp(offset, cellBytesTotal) & ~1u;
		u8* cells = fits != 0 ? vram.span(offset, fits) : nullptr;
		for (u32 i = 0; i + 1 < fits; i += 2)
		{
			cells[i] = static_cast<u8>(BlankCell & 0xFF);
			cells[i + 1] = static_cast<u8>(BlankCell >> 8);
		}
	}

	u32 TextPlane::read(u32 offset) const noexcept
	{
		switch (offset)
		{
			case EnableRegister: return _enabled ? 1u : 0u;
			case ColsRegister: return _cols;
			case RowsRegister: return _rows;
			case CellsBaseRegister: return _cellsBasePending;
			case CellFormatRegister: return _cellFormat;
			case FontBaseRegister: return _fontBasePending;
			case GlyphCountRegister: return _glyphCount;
			case PaletteBaseRegister: return _paletteBasePending;
			case CursorXRegister: return _cursorX;
			case CursorYRegister: return _cursorY;
			case CursorShapeRegister: return _cursorShape;
			case ScrollbackBaseRegister: return _scrollbackBase;
			case ScrollbackLinesRegister: return _scrollbackLines;
			case ScrollYRegister: return _scrollY;
			case ScrollbackHeadRegister: return _scrollbackHead;
			case ScrollbackCountRegister: return _scrollbackCount;
			default: return 0;
		}
	}

	void TextPlane::write(u32 offset, u32 value) noexcept
	{
		switch (offset)
		{
			case EnableRegister: _enabled = (value & 1u) != 0; break;
			case CellsBaseRegister: _cellsBasePending = value; break;
			case CellFormatRegister: _cellFormat = value & 1u; break;
			case FontBaseRegister: _fontBasePending = value; break;
			case GlyphCountRegister: _glyphCount = std::clamp<u32>(value, 1, FontGlyphCount); break;
			case PaletteBaseRegister: _paletteBasePending = value; break;
			case CursorXRegister: _cursorX = value; break;
			case CursorYRegister: _cursorY = value; break;
			case CursorShapeRegister: _cursorShape = value & (3u | CursorBlink); break;
			case ScrollbackBaseRegister: _scrollbackBase = value; break;
			case ScrollbackLinesRegister: _scrollbackLines = value; break;
			case ScrollYRegister: setScrollY(value); break;
			case ScrollbackHeadRegister: _scrollbackHead = _scrollbackLines == 0 ? 0 : value % _scrollbackLines; break;
			case ScrollbackCountRegister: setScrollback(_scrollbackHead, std::min(value, _scrollbackLines)); break;
			default: break;
		}
	}

	void TextPlane::applyPending() noexcept
	{
		_cellsBase = _cellsBasePending;
		_fontBase = _fontBasePending;
		_paletteBase = _paletteBasePending;
	}

	u32 TextPlane::shownCell(const vm::Vram& vram, u32 col, u32 row) const noexcept
	{
		u32 address;
		if (row >= _scrollY)
			address = cellAddress(col, row - _scrollY);
		else
		{
			// Row `row` of the screen shows the ring's row `_scrollY - row` back from the newest.
			const u32 back = _scrollY - row;
			if (back > _scrollbackCount || _scrollbackLines == 0)
				return 0;
			const u32 line = (_scrollbackHead + _scrollbackLines - back % _scrollbackLines) % _scrollbackLines;
			address = scrollbackRowAddress(line) + col * cellBytes();
		}
		const u32 bytes = cellBytes();
		if (!vram.backs(address, bytes))
			return 0;
		const u32 offset = address - vm::Vram::BaseValue;
		return bytes == 4 ? vram.read<u32>(offset) : vram.read<u16>(offset);
	}

	std::string TextPlane::toText(const vm::Vram& vram) const
	{
		std::string out;
		for (u32 row = 0; row < _rows; ++row)
		{
			std::string line;
			for (u32 col = 0; col < _cols; ++col)
			{
				const u32 address = cellAddress(col, row);
				const u32 cell = vram.backs(address, cellBytes()) ? (cellBytes() == 4 ? vram.read<u32>(address - vm::Vram::BaseValue) : vram.read<u16>(address - vm::Vram::BaseValue)) : 0u;
				appendUtf8(line, glyphCodePoint(cell & 0xFF));
			}
			line.erase(line.find_last_not_of(' ') == std::string::npos ? 0 : line.find_last_not_of(' ') + 1);
			out += line;
			out += '\n';
		}
		return out;
	}

	u32 glyphCodePoint(u32 glyph) noexcept
	{
		glyph &= 0xFF;
		if (glyph >= 0x80 && glyph < 0xA0)
			return BoxGlyphCodePoints[glyph - 0x80];
		if (glyph < 0x20 || glyph == 0x7F)
			return ' ';
		return glyph;
	}

	u32 glyphFor(u32 codePoint) noexcept
	{
		if ((codePoint >= 0x20 && codePoint < 0x7F) || (codePoint >= 0xA0 && codePoint <= 0xFF))
			return codePoint;
		for (u32 i = 0; i < BoxGlyphCodePoints.size(); ++i)
			if (BoxGlyphCodePoints[i] == codePoint)
				return 0x80 + i;
		return '?';
	}
}

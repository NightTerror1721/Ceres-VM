#include <ceres/devices/video/display.h>
#include <algorithm>
#include <cstring>

namespace ceres::devices
{
	u32 DisplayDevice::readUnsignedWord(Address offset)
	{
		if (offset == WidthRegister) return _width;
		if (offset == HeightRegister) return _height;
		if (offset == ModeRegister) return _mode;
		if (offset == ScrollXRegister) return _scrollX;
		if (offset == ScrollYRegister) return _scrollY;
		return 0;
	}

	void DisplayDevice::writeWord(Address offset, u32 value)
	{
		if (offset == WidthRegister) { resize(value, _height); return; }
		if (offset == HeightRegister) { resize(_width, value); return; }

		if (offset == CommandRegister)
		{
			if (value == CommandClear)
				clear();
			else if (value == CommandPresent)
				present();
			return;
		}

		if (offset == DataRegister)
		{
			// One pixel at a time, for a program that would rather poke than blit.
			if (_mode == ModeIndexed)
			{
				if (_cursor < _indexed.size())
					_indexed[_cursor++] = static_cast<u8>(value);
			}
			else if (_cursor < _pixels.size())
				_pixels[_cursor++] = value;
			return;
		}
		if (offset == ModeRegister)
		{
			_mode = value == ModeIndexed ? ModeIndexed : ModeRgb32;
			_indexed.assign(_mode == ModeIndexed ? _pixels.size() : 0, 0);
			_cursor = 0;
			_composed = false;                        // the frame was composed for the other mode
			return;
		}
		if (offset == PaletteIndexRegister) { _paletteIndex = value & 255u; return; }
		if (offset == PaletteDataRegister) { _palette[_paletteIndex] = value; _paletteIndex = (_paletteIndex + 1) & 255u; return; }
		if (offset == ScrollXRegister) { _scrollX = value % _width; return; }
		if (offset == ScrollYRegister) { _scrollY = value % _height; return; }

		if (offset == BlockAddressRegister) { _blockAddress = value; return; }
		if (offset == BlockLengthRegister) { _blockLength = value; return; }
		if (offset == BlockCommandRegister && value == BlockCommandWrite)
			blockWrite(Address(_blockAddress), _blockLength);
	}

	void DisplayDevice::resize(u32 width, u32 height)
	{
		// A surface of nothing, or one larger than any screen, is a typo rather than a request.
		if (width == 0 || height == 0 || width > MaxWidth || height > MaxHeight)
			return;

		_width = width;
		_height = height;
		_pixels.assign(static_cast<usize>(_width) * _height, 0);
		if (_mode == ModeIndexed)
			_indexed.assign(_pixels.size(), 0);
		_scrollX %= _width;
		_scrollY %= _height;
		_cursor = 0;
		_composed = false;                            // a composed frame of the old size would be read at the new one
	}

	void DisplayDevice::clear()
	{
		std::fill(_pixels.begin(), _pixels.end(), 0u);
		std::fill(_indexed.begin(), _indexed.end(), u8{ 0 });
		_cursor = 0;
		_composed = false;
	}

	void DisplayDevice::blockWrite(Address ramAddress, u32 size)
	{
		if (size == 0 || _cursor >= _pixels.size())
			return;

		const u32 clampSize = memory().clampBlockSize(ramAddress, size);
		if (clampSize == 0)
			return;

		if (_mode == ModeIndexed)                  // a byte a pixel
		{
			const u32 count = std::min(clampSize, static_cast<u32>(_indexed.size() - _cursor));
			const auto bytes = memory().peekBytes(ramAddress, count);
			std::memcpy(_indexed.data() + _cursor, bytes.data(), count);
			_cursor += count;
			return;
		}

		// Each pixel is four bytes; move whole pixels only.
		const u32 pixelBytes = clampSize & ~3u;
		const u32 available = static_cast<u32>(_pixels.size() - _cursor);
		const u32 pixels = std::min(pixelBytes / 4u, available);

		const auto bytes = memory().peekBytes(ramAddress, pixels * 4u);
		std::memcpy(_pixels.data() + _cursor, bytes.data(), pixels * 4u);
		_cursor += pixels;
	}

	void DisplayDevice::present()
	{
		_cursor = 0;
		++_presentCount;

		// Through the palette and the scroll, when either is in play; the pixels as they are otherwise.
		_composed = _mode == ModeIndexed || _scrollX != 0 || _scrollY != 0;
		if (_composed)
		{
			_frame.resize(_pixels.size());
			for (u32 y = 0; y < _height; ++y)
			{
				const u32 sy = (y + _scrollY) % _height;
				for (u32 x = 0; x < _width; ++x)
				{
					const usize from = static_cast<usize>(sy) * _width + (x + _scrollX) % _width;
					_frame[static_cast<usize>(y) * _width + x] = _mode == ModeIndexed ? _palette[_indexed[from]] : _pixels[from];
				}
			}
		}

		if (_sink)
			_sink(_width, _height, frame());
	}
}

#include <ceres/devices/video/bitmap_plane.h>
#include <ceres/devices/video/formats.h>

#include <algorithm>

namespace ceres::devices::video
{
	void BitmapPlane::reset(u32 width, u32 height, u32 base, u32 paletteBase, usize vramSize) noexcept
	{
		_enabled = false;
		_format = static_cast<u32>(PixelFormat::Xrgb8888);
		_width = width;
		_height = height;
		_pitch = width * 4;
		_scrollX = _scrollY = 0;
		_buffers = 1;
		_paletteBase = _shownPalette = paletteBase;
		_queued = false;

		// Three pictures of the screen's size after `base`, each at a multiple of 256, as far as the VRAM holds them.
		const u64 size = (u64{ _pitch } * height + 255) & ~u64{ 255 };
		const u64 end = u64{ vm::Vram::BaseValue } + vramSize;
		const auto fits = [&](u64 at) { return at + size <= end ? static_cast<u32>(at) : 0u; };
		_base = _shownBase = fits(base);
		_backBase = _base != 0 ? fits(u64{ base } + size) : 0;
		_spareBase = _backBase != 0 ? fits(u64{ base } + 2 * size) : 0;
	}

	u32 BitmapPlane::read(u32 offset) const noexcept
	{
		switch (offset)
		{
			case EnableRegister: return _enabled ? 1u : 0u;
			case BaseRegister: return _base;
			case BackBaseRegister: return _backBase;
			case PitchRegister: return _pitch;
			case FormatRegister: return _format;
			case WidthRegister: return _width;
			case HeightRegister: return _height;
			case ScrollXRegister: return _scrollX;
			case ScrollYRegister: return _scrollY;
			case BuffersRegister: return _buffers;
			case PaletteBaseRegister: return _paletteBase;
			case SpareBaseRegister: return _spareBase;
			default: return 0;
		}
	}

	void BitmapPlane::write(u32 offset, u32 value) noexcept
	{
		switch (offset)
		{
			case EnableRegister: _enabled = (value & 1u) != 0; break;
			case BaseRegister: _base = value; break;
			case BackBaseRegister: _backBase = value; break;
			case PitchRegister: _pitch = value; break;
			case FormatRegister: if (isPixelFormat(value)) _format = value; break;
			case WidthRegister: _width = value; break;
			case HeightRegister: _height = value; break;
			case ScrollXRegister: _scrollX = value; break;
			case ScrollYRegister: _scrollY = value; break;
			case BuffersRegister: _buffers = std::clamp<u32>(value, 1, 3); _queued = false; break;
			case PaletteBaseRegister: _paletteBase = value; break;
			case SpareBaseRegister: _spareBase = value; break;
			default: break;
		}
	}

	void BitmapPlane::present() noexcept
	{
		if (_buffers != 3)
			return;
		if (!_queued)
		{
			_queuedBase = _backBase;
			_backBase = _spareBase;
			_queued = true;
		}
		else
			std::swap(_queuedBase, _backBase);
	}

	void BitmapPlane::applyPending() noexcept
	{
		if (_buffers == 2)
			std::swap(_base, _backBase);
		else if (_buffers == 3 && _queued)
		{
			_spareBase = _base;
			_base = _queuedBase;
			_queued = false;
		}
		_shownBase = _base;
		_shownPalette = _paletteBase;
	}
}

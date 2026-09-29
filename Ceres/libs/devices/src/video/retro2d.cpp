#include <ceres/devices/video/retro2d.h>

namespace ceres::devices::video
{
	void Retro2D::reset() noexcept
	{
		*this = Retro2D{};
	}

	bool Retro2D::anyEnabled() const noexcept
	{
		for (const TileLayer& layer : _layers)
			if (layer.enabled())
				return true;
		return false;
	}

	u32 Retro2D::read(u32 offset) const noexcept
	{
		if (offset >= LayerRegisters && offset < LayerRegisters + LayerCount * LayerStride)
		{
			const TileLayer& layer = _layers[(offset - LayerRegisters) / LayerStride];
			switch ((offset - LayerRegisters) % LayerStride)
			{
				case LayerControlRegister: return layer.control;
				case LayerMapBaseRegister: return layer.mapBase;
				case LayerTileBaseRegister: return layer.tileBase;
				case LayerScrollXRegister: return layer.scrollX;
				case LayerScrollYRegister: return layer.scrollY;
				case LayerLineScrollBaseRegister: return layer.lineScrollBase;
				default: return 0;
			}
		}
		switch (offset)
		{
			case TilePaletteBaseRegister: return _tilePaletteBase;
			case SpritePaletteBaseRegister: return _spritePaletteBase;
			default: return 0;
		}
	}

	void Retro2D::write(u32 offset, u32 value) noexcept
	{
		if (offset >= LayerRegisters && offset < LayerRegisters + LayerCount * LayerStride)
		{
			TileLayer& layer = _layers[(offset - LayerRegisters) / LayerStride];
			switch ((offset - LayerRegisters) % LayerStride)
			{
				case LayerControlRegister: layer.control = value & LayerControl::Mask; break;
				case LayerMapBaseRegister: layer.mapBase = value; break;
				case LayerTileBaseRegister: layer.tileBase = value; break;
				case LayerScrollXRegister: layer.scrollX = value; break;
				case LayerScrollYRegister: layer.scrollY = value; break;
				case LayerLineScrollBaseRegister: layer.lineScrollBase = value; break;
				default: break;
			}
			return;
		}
		switch (offset)
		{
			case TilePaletteBaseRegister: _tilePaletteBase = value; break;
			case SpritePaletteBaseRegister: _spritePaletteBase = value; break;
			default: break;
		}
	}
}

#include <ceres/devices/video/blitter.h>
#include <algorithm>
#include <vector>

namespace ceres::devices
{
	namespace
	{
		// Every register of the device (plan/v2 SPEC 5.3), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x00, "Command",        RegisterAccess::Write,     0x0, false, "The operation." },
			{ 0x04, "DstAddress",     RegisterAccess::Write,     0x0, false, "The destination's first pixel." },
			{ 0x08, "DstStride",      RegisterAccess::Write,     0x0, false, "Bytes between its rows." },
			{ 0x0C, "SrcAddress",     RegisterAccess::Write,     0x0, false, "The source's first pixel." },
			{ 0x10, "SrcStride",      RegisterAccess::Write,     0x0, false, "Bytes between the source rows." },
			{ 0x14, "Width",          RegisterAccess::Write,     0x0, false, "Pixels (of the source, for a copy)" },
			{ 0x18, "Height",         RegisterAccess::Write,     0x0, false, "Rows (of the source, for a copy)." },
			{ 0x1C, "Color",          RegisterAccess::Write,     0x0, false, "The fill colour, or the key." },
			{ 0x20, "Scale",          RegisterAccess::Write,     0x0, false, "1..8, for CopyScaled." },
			{ 0x24, "PaletteAddress", RegisterAccess::Write,     0x0, false, "256 RGB32 entries in RAM." },
			{ 0x28, "Control",        RegisterAccess::ReadWrite, 0x0, false, "Bit 0 interrupt when done." },
			{ 0x2C, "Status",         RegisterAccess::Read,      0x0, false, "Bit 0 the last operation failed." },
			{ 0x30, "Pixels",         RegisterAccess::Read,      0x0, false, "Pixels the last one wrote." },
		};
	}

	void BlitterDevice::reset()
	{
		_control = 0;
		_status = 0;
		_pixels = 0;
	}

	std::span<u8> BlitterDevice::row(u32 address, u32 bytes)
	{
		if (bytes == 0 || memory().clampBlockSize(Address(address), bytes) != bytes)
			return {};
		return memory().peekMutBytes(Address(address), bytes);
	}

	std::span<u8> BlitterDevice::rowOf(u32 base, u32 y, u32 stride, u32 bytes)
	{
		const u64 address = static_cast<u64>(base) + static_cast<u64>(y) * stride;
		if (address > 0xFFFFFFFFull)
			return {};
		return row(static_cast<u32>(address), bytes);
	}

	bool BlitterDevice::fill()
	{
		for (u32 y = 0; y < _height; ++y)
		{
			auto d = rowOf(_dst, y, _dstStride, _width * 4u);
			if (d.empty())
				return false;
			for (u32 x = 0; x < _width; ++x)
				store(d.data() + x * 4u, _color);
			_pixels += _width;
		}
		return true;
	}

	bool BlitterDevice::copy(bool keyed)
	{
		// Rows bottom-up when the destination starts after the source in memory, as memmove would.
		const bool backwards = _dst > _src;
		for (u32 i = 0; i < _height; ++i)
		{
			const u32 y = backwards ? _height - 1 - i : i;
			auto s = rowOf(_src, y, _srcStride, _width * 4u);
			auto d = rowOf(_dst, y, _dstStride, _width * 4u);
			if (s.empty() || d.empty())
				return false;
			if (!keyed)
			{
				std::memmove(d.data(), s.data(), _width * 4u);
				_pixels += _width;
				continue;
			}
			std::vector<u8> line(s.begin(), s.end());   // the row as it was, whatever the overlap
			for (u32 x = 0; x < _width; ++x)
			{
				const u32 pixel = load(line.data() + x * 4u);
				if (pixel == _color)
					continue;
				store(d.data() + x * 4u, pixel);
				++_pixels;
			}
		}
		return true;
	}

	bool BlitterDevice::copyScaled()
	{
		const u32 scale = std::clamp(_scale, 1u, MaxScale);
		for (u32 y = 0; y < _height; ++y)
		{
			auto s = rowOf(_src, y, _srcStride, _width * 4u);
			if (s.empty())
				return false;
			std::vector<u8> line(s.begin(), s.end());
			for (u32 k = 0; k < scale; ++k)
			{
				auto d = rowOf(_dst, y * scale + k, _dstStride, _width * scale * 4u);
				if (d.empty())
					return false;
				for (u32 x = 0; x < _width; ++x)
				{
					const u32 pixel = load(line.data() + x * 4u);
					for (u32 j = 0; j < scale; ++j)
						store(d.data() + (x * scale + j) * 4u, pixel);
				}
				_pixels += _width * scale;
			}
		}
		return true;
	}

	bool BlitterDevice::copyIndexed(bool keyed)
	{
		auto table = row(_palette, 256u * 4u);
		if (table.empty())
			return false;
		// The palette and each source row as they were before this copy wrote anything: either may lie in the
		// destination.
		const std::vector<u8> palette(table.begin(), table.end());
		for (u32 y = 0; y < _height; ++y)
		{
			auto s = rowOf(_src, y, _srcStride, _width);
			auto d = rowOf(_dst, y, _dstStride, _width * 4u);
			if (s.empty() || d.empty())
				return false;
			const std::vector<u8> line(s.begin(), s.end());
			for (u32 x = 0; x < _width; ++x)
			{
				const u8 index = line[x];
				if (keyed && index == (_color & 0xFFu))
					continue;
				store(d.data() + x * 4u, load(palette.data() + index * 4u));
				++_pixels;
			}
		}
		return true;
	}

	void BlitterDevice::command(u32 value)
	{
		_pixels = 0;
		bool ok = true;
		switch (value)
		{
		case CommandFill: ok = fill(); break;
		case CommandCopy: ok = copy(false); break;
		case CommandCopyKeyed: ok = copy(true); break;
		case CommandCopyScaled: ok = copyScaled(); break;
		case CommandCopyIndexed: ok = copyIndexed(false); break;
		case CommandCopyIndexedKeyed: ok = copyIndexed(true); break;
		default: ok = false; break;
		}
		_status = ok ? 0u : StatusError;
		if (_control & ControlInterrupt)
			raiseInterrupt(Interrupt);
	}

	u32 BlitterDevice::read(Address offset)
	{
		if (offset == ControlRegister) return _control;
		if (offset == StatusRegister) return _status;
		if (offset == PixelsRegister) return _pixels;
		return 0;
	}

	void BlitterDevice::write(Address offset, u32 value)
	{
		if (offset == CommandRegister) command(value);
		else if (offset == DstAddressRegister) _dst = value;
		else if (offset == DstStrideRegister) _dstStride = value;
		else if (offset == SrcAddressRegister) _src = value;
		else if (offset == SrcStrideRegister) _srcStride = value;
		else if (offset == WidthRegister) _width = std::min(value, 4096u);
		else if (offset == HeightRegister) _height = std::min(value, 4096u);
		else if (offset == ColorRegister) _color = value;
		else if (offset == ScaleRegister) _scale = value;
		else if (offset == PaletteAddressRegister) _palette = value;
		else if (offset == ControlRegister) _control = value;
	}

	const RegisterMap& BlitterDevice::registers() const
	{
		static constexpr RegisterMap map{ "blitter", Registers };
		return map;
	}
}

#pragma once

// A 2D blitter: rectangle operations on RGB32 surfaces in RAM, done by the host instead of the program's
// instructions - what a game's frame spends most of its time on. A surface is an address and a stride (bytes
// from one row to the next); an operation is a width and a height, and a command.
//
//   Fill        dst rectangle = Color
//   Copy        src rectangle -> dst (overlapping surfaces are fine: rows and pixels go in the safe order)
//   CopyKeyed   the same, leaving out every source pixel equal to Color (sprites)
//   CopyScaled  every source pixel becomes a Scale x Scale block (1..8) of the destination
//   CopyIndexed 8-bit source pixels, each looked up in the 256-entry RGB32 palette at PaletteAddress;
//               CopyIndexedKeyed leaves out the index in Color
//
// An operation is carried out at once; the Pixels register says how many destination pixels it wrote, and the
// Status register's error bit is set when a row of either surface ran outside RAM (the operation stops there).
// With Control bit 0 the device raises its interrupt (25) when an operation is done, for a program that sleeps
// meanwhile.

#include <ceres/vm/mmio_bus.h>
#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

namespace ceres::devices
{
	using namespace vm;

	class BlitterDevice final : public IODevice
	{
	public:
		static inline constexpr Address CommandRegister = Address(0x00);        // Write: the operation
		static inline constexpr Address DstAddressRegister = Address(0x04);     // Write: the destination's first pixel
		static inline constexpr Address DstStrideRegister = Address(0x08);      // Write: bytes between its rows
		static inline constexpr Address SrcAddressRegister = Address(0x0C);
		static inline constexpr Address SrcStrideRegister = Address(0x10);
		static inline constexpr Address WidthRegister = Address(0x14);          // Write: pixels (of the source, for a copy)
		static inline constexpr Address HeightRegister = Address(0x18);
		static inline constexpr Address ColorRegister = Address(0x1C);          // Write: the fill colour, or the key
		static inline constexpr Address ScaleRegister = Address(0x20);          // Write: 1..8, for CopyScaled
		static inline constexpr Address PaletteAddressRegister = Address(0x24); // Write: 256 RGB32 entries in RAM
		static inline constexpr Address ControlRegister = Address(0x28);        // Read/write: bit 0 interrupt when done
		static inline constexpr Address StatusRegister = Address(0x2C);         // Read: bit 0 the last operation failed
		static inline constexpr Address PixelsRegister = Address(0x30);         // Read: pixels the last one wrote

		static inline constexpr u32 CommandFill = 1;
		static inline constexpr u32 CommandCopy = 2;
		static inline constexpr u32 CommandCopyKeyed = 3;
		static inline constexpr u32 CommandCopyScaled = 4;
		static inline constexpr u32 CommandCopyIndexed = 5;
		static inline constexpr u32 CommandCopyIndexedKeyed = 6;

		static inline constexpr u32 ControlInterrupt = 1u << 0;
		static inline constexpr u32 StatusError = 1u << 0;
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt9;   // 25

		static inline constexpr u32 MaxScale = 8;

	private:
		u32 _dst = 0, _dstStride = 0, _src = 0, _srcStride = 0;
		u32 _width = 0, _height = 0, _color = 0, _scale = 1, _palette = 0;
		u32 _control = 0, _status = 0, _pixels = 0;

	public:
		BlitterDevice() = default;
		BlitterDevice(const BlitterDevice&) = delete;
		BlitterDevice(BlitterDevice&&) = delete;
		~BlitterDevice() override = default;

		BlitterDevice& operator=(const BlitterDevice&) = delete;
		BlitterDevice& operator=(BlitterDevice&&) = delete;

		void attachTo(MmioBus& bus) { bus.attach(default_mmio::Blitter, *this); }
		void detachFrom(MmioBus& bus) { bus.detach(default_mmio::Blitter); }

		void reset() override
		{
			_control = 0;
			_status = 0;
			_pixels = 0;
		}

	private:
		// A row of `bytes` at `address`, entirely in RAM; empty when it is not.
		std::span<u8> row(u32 address, u32 bytes)
		{
			if (bytes == 0 || memory().clampBlockSize(Address(address), bytes) != bytes)
				return {};
			return memory().peekMutBytes(Address(address), bytes);
		}

		// Row y of a surface at `base` with `stride` bytes a row. A stride that would carry the address past 4 GiB is
		// out of RAM like any other, rather than wrapping round to some address inside it.
		std::span<u8> rowOf(u32 base, u32 y, u32 stride, u32 bytes)
		{
			const u64 address = static_cast<u64>(base) + static_cast<u64>(y) * stride;
			if (address > 0xFFFFFFFFull)
				return {};
			return row(static_cast<u32>(address), bytes);
		}

		static u32 load(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }
		static void store(u8* p, u32 v) { std::memcpy(p, &v, 4); }

		bool fill()
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

		bool copy(bool keyed)
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

		bool copyScaled()
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

		bool copyIndexed(bool keyed)
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

		void command(u32 value)
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

	public:
		u32 readUnsignedWord(Address offset) override
		{
			if (offset == ControlRegister) return _control;
			if (offset == StatusRegister) return _status;
			if (offset == PixelsRegister) return _pixels;
			return 0;
		}
		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		void writeWord(Address offset, u32 value) override
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
		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }
	};
}

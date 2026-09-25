#pragma once

// A pixel framebuffer, the display half of the "consola retro" the roadmap wants: a grid of RGB32
// pixels a program draws into and then presents. It leaves the text FramebufferDevice untouched
// for text output, and is the natural SDL surface - Fase 2 of the SDL3 plan blits these pixels
// into a texture.
//
// Two retro touches. In the INDEXED mode (ModeRegister = 1) a pixel is one byte, an index into a palette of
// 256 RGB32 colours the program sets through PaletteIndex/PaletteData: a quarter of the memory to fill, and
// palette tricks (a colour changed everywhere at once) for free. And SCROLL: ScrollX and ScrollY shift what is
// presented, wrapping round, so a background larger than the screen - or a scrolling one - costs no copy.

#include <ceres/vm/mmio_bus.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <span>
#include <vector>

namespace ceres::devices
{
	using namespace vm;

	class DisplayDevice final : public IODevice
	{
	public:
		static inline constexpr Address CommandRegister = Address(0x00);      // Write: 1 = clear, 2 = present
		static inline constexpr Address WidthRegister = Address(0x04);        // Read/write: pixel columns
		static inline constexpr Address HeightRegister = Address(0x08);       // Read/write: pixel rows
		static inline constexpr Address DataRegister = Address(0x0C);         // Write: one pixel (RGB32, or an index) at the cursor
		static inline constexpr Address ModeRegister = Address(0x10);         // Read/write: 0 RGB32, 1 indexed (8 bits a pixel)
		static inline constexpr Address PaletteIndexRegister = Address(0x14); // Write: the palette entry PaletteData sets next
		static inline constexpr Address PaletteDataRegister = Address(0x18);  // Write: RGB32 for that entry, then the next
		static inline constexpr Address ScrollXRegister = Address(0x1C);      // Read/write: the column shown at the left edge
		static inline constexpr Address ScrollYRegister = Address(0x20);      // Read/write: the row shown at the top
		static inline constexpr Address BlockAddressRegister = Address(0xF0); // Write: RAM address to blit from
		static inline constexpr Address BlockLengthRegister = Address(0xF4);  // Write: bytes to blit (a multiple of 4)
		static inline constexpr Address BlockCommandRegister = Address(0xF8); // Write: 2 = blit pixels into the buffer

		static inline constexpr u32 CommandClear = 1;
		static inline constexpr u32 CommandPresent = 2;
		static inline constexpr u32 ModeRgb32 = 0;
		static inline constexpr u32 ModeIndexed = 1;
		static inline constexpr u32 BlockCommandWrite = 2;

		static inline constexpr u32 MaxWidth = 1280;
		static inline constexpr u32 MaxHeight = 720;

		// Where a presented frame goes: its dimensions plus the RGB32 pixels (0x00RRGGBB, top byte
		// ignored). Without a sink nothing happens, so a headless build stays silent; an SDL host
		// installs one to upload the pixels into a texture.
		using FrameSink = std::function<void(u32 width, u32 height, std::span<const u32> pixels)>;

	private:
		u32 _width = 320;
		u32 _height = 200;
		std::vector<u32> _pixels;
		std::vector<u8> _indexed;        // the indexed mode's pixels
		std::vector<u32> _frame;         // what was presented, when it is not _pixels as they are
		bool _composed = false;
		u32 _mode = ModeRgb32;
		u32 _palette[256] = {};
		u32 _paletteIndex = 0;
		u32 _scrollX = 0;
		u32 _scrollY = 0;
		u32 _cursor = 0; // Next pixel index for a DataRegister write, in pixels
		FrameSink _sink;
		u32 _blockAddress = 0;
		u32 _blockLength = 0;
		u64 _presentCount = 0;

	public:
		DisplayDevice() : _pixels(static_cast<usize>(_width) * _height, 0) {}

		DisplayDevice(const DisplayDevice&) = delete;
		DisplayDevice(DisplayDevice&&) = delete;
		~DisplayDevice() override = default;

		DisplayDevice& operator=(const DisplayDevice&) = delete;
		DisplayDevice& operator=(DisplayDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Display, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Display);
		}

		void setFrameSink(FrameSink sink) { _sink = std::move(sink); }
		void clearFrameSink() { _sink = nullptr; }

		u32 width() const noexcept { return _width; }
		u32 height() const noexcept { return _height; }
		std::span<const u32> pixels() const noexcept { return _pixels; }
		// The last frame presented, as shown: through the palette, and scrolled. A window draws this.
		std::span<const u32> frame() const noexcept { return _composed ? std::span<const u32>(_frame) : std::span<const u32>(_pixels); }
		u32 mode() const noexcept { return _mode; }

		// How many frames the program has presented. A window that also shows the text framebuffer compares
		// it with what it saw last, to tell that the pixels are what the program showed most recently.
		u64 presentCount() const noexcept { return _presentCount; }

	public:
		u32 readUnsignedWord(Address offset) override
		{
			if (offset == WidthRegister) return _width;
			if (offset == HeightRegister) return _height;
			if (offset == ModeRegister) return _mode;
			if (offset == ScrollXRegister) return _scrollX;
			if (offset == ScrollYRegister) return _scrollY;
			return 0;
		}
		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		void writeWord(Address offset, u32 value) override
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
		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }

	private:
		void resize(u32 width, u32 height)
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

		void clear()
		{
			std::fill(_pixels.begin(), _pixels.end(), 0u);
			std::fill(_indexed.begin(), _indexed.end(), u8{ 0 });
			_cursor = 0;
			_composed = false;
		}

		// A run of pixels starting where the last write left off, so a whole frame is one trigger.
		void blockWrite(Address ramAddress, u32 size)
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

		void present()
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
	};
}

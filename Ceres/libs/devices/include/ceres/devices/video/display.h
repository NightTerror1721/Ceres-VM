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
		u32 read(Address offset) override;
		void write(Address offset, u32 value) override;

	private:
		void resize(u32 width, u32 height);

		void clear();

		// A run of pixels starting where the last write left off, so a whole frame is one trigger.
		void blockWrite(Address ramAddress, u32 size);

		void present();
	};
}

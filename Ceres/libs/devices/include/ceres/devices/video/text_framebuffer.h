#pragma once

// The text framebuffer: a grid of characters a program draws into and then shows. Replaced by the GPU's text
// plane (level V0) in plan/v2 F5.

#include <ceres/vm/mmio_bus.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ceres::devices
{
	using namespace vm;

	// A grid of characters that a program draws into and then shows. Not pixels: this machine has
	// no window to put them in, and a text framebuffer is the thing the tutorial's games actually
	// want - a board that is redrawn whole rather than a terminal that is scrolled.
	class FramebufferDevice final : public IODevice
	{
	public:
		static inline constexpr Address CommandRegister = Address(0x00);
		static inline constexpr Address WidthRegister = Address(0x04);
		static inline constexpr Address HeightRegister = Address(0x08);
		static inline constexpr Address DataRegister = Address(0x0C);
		static inline constexpr Address ModeRegister = Address(0x10);   // Read/write: where a presented frame goes - ModeAuto, ModeTerminal or ModeWindow.
		static inline constexpr Address OutputRegister = Address(0x14); // Read-only: where it goes right now - OutputTerminal or OutputWindow.
		static inline constexpr Address BlockAddressRegister = Address(0xF0);
		static inline constexpr Address BlockLengthRegister = Address(0xF4);
		static inline constexpr Address BlockCommandRegister = Address(0xF8);

		static inline constexpr u32 CommandClear = 1;
		static inline constexpr u32 CommandPresent = 2;

		// A frame is shown in the host's window when it has one (the default, ModeAuto: this is a machine with
		// a screen), or as text on the terminal (ModeTerminal). ModeWindow asks for the window explicitly and
		// is the same as ModeAuto where there is one; where there is none, both fall back to the terminal, so a
		// program that asks for the window still shows something. OutputRegister says which it is.
		static inline constexpr u32 ModeAuto = 0;
		static inline constexpr u32 ModeTerminal = 1;
		static inline constexpr u32 ModeWindow = 2;
		static inline constexpr u32 OutputTerminal = 1;
		static inline constexpr u32 OutputWindow = 2;

		// A cell is a character and an attribute. The DataRegister word holds the character in bits
		// 7:0 and the attribute in bits 15:8. Attribute 0 means the terminal's own colours;
		// otherwise the low nibble is the foreground and the high nibble the background, in the
		// order of the ANSI palette: 0 black, 1 red, 2 green, 3 yellow, 4 blue, 5 magenta, 6 cyan,
		// 7 white, and 8-15 the bright versions of the same.
		static inline constexpr u32 AttributeShift = 8;

		static inline constexpr u32 MaxWidth = 200;
		static inline constexpr u32 MaxHeight = 100;

		// Block commands: DataRegister writes one cell at a time; the block form (BlockCommandWrite
		// only - a framebuffer is never read back in bulk) writes a run in one trigger.
		static inline constexpr u32 BlockCommandWrite = 2;
		// The same run-in-one-trigger for the attribute plane, which has a cursor of its own: one
		// byte per cell, in the same row-major order as the characters.
		static inline constexpr u32 BlockCommandWriteAttributes = 3;

		using PresentSink = std::function<void(std::string_view)>;

		// A frame as it was when the program presented it: what a window draws from, so the program can go on
		// changing the grid without tearing what is shown.
		struct Frame
		{
			u32 width = 0;
			u32 height = 0;
			std::vector<u8> cells;
			std::vector<u8> attributes;
		};

	private:
		u32 _mode = ModeAuto;
		bool _windowHost = false;
		bool _hasWindowFrame = false;
		Frame _windowFrame;
		u32 _width = 40;
		u32 _height = 20;
		std::vector<u8> _cells;
		std::vector<u8> _attributes;
		u32 _cursor = 0; // Where the next block write lands, in cells
		u32 _attributeCursor = 0;
		PresentSink _sink;
		u32 _blockAddress = 0;
		u32 _blockLength = 0;

	public:
		FramebufferDevice() : _cells(static_cast<usize>(_width) * _height, ' '), _attributes(_cells.size(), 0) {}

		FramebufferDevice(const FramebufferDevice&) = delete;
		FramebufferDevice(FramebufferDevice&&) = delete;
		~FramebufferDevice() override = default;

		FramebufferDevice& operator=(const FramebufferDevice&) = delete;
		FramebufferDevice& operator=(FramebufferDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Framebuffer, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Framebuffer);
		}

		// Where a presented frame goes. Without one it goes to stdout, which is what the CLI wants
		// and what a test does not.
		void setPresentSink(PresentSink sink) { _sink = std::move(sink); }

		// The host has a window that shows frames. Without one every frame goes to the terminal, whatever the
		// mode; with one it goes there unless the program asked for the terminal.
		void setWindowHost(bool hasWindow) noexcept { _windowHost = hasWindow; }
		bool hasWindowHost() const noexcept { return _windowHost; }
		u32 mode() const noexcept { return _mode; }
		u32 output() const noexcept { return (_mode != ModeTerminal && _windowHost) ? OutputWindow : OutputTerminal; }

		// The grid as it is now.
		Frame snapshot() const { return Frame{ _width, _height, _cells, _attributes }; }

		// The frame the program presented for the window, if there is one the host has not taken yet. The host
		// calls this between slices of instructions; a program that presents twice in a slice shows the last.
		bool takeWindowFrame(Frame& out);

		// The host could not show a frame in a window after all (no display, say): give this one and every
		// later one to the terminal.
		void fallBackToTerminal();

		u32 width() const noexcept { return _width; }
		u32 height() const noexcept { return _height; }
		std::span<const u8> cells() const noexcept { return _cells; }
		std::span<const u8> attributes() const noexcept { return _attributes; }

		// The grid as text, rows separated by newlines. What `present` sends, and what a test can
		// compare against without going through a terminal.
		std::string toText() const;

	public:
		// The grid with its colours, as the escape sequences a terminal understands. A cell with
		// attribute 0 uses the terminal's own colours, and every row ends back on them, so a frame
		// never leaves the terminal painted.
		std::string toAnsiText() const;

		bool hasAttributes() const noexcept
		{
			return std::ranges::any_of(_attributes, [](u8 attribute) { return attribute != 0; });
		}

	private:
		// A cell as text. Its byte is Latin-1, so 0xA0-0xFF is the code point of the same number, which UTF-8
		// writes in two bytes. A cell nobody wrote is a space, and a control character (below 0x20, or 0x7F to
		// 0x9F) would move the terminal's own cursor rather than showing anything, so it is one too.
		static void appendCell(std::string& out, u8 cell);

		static void appendSgr(std::string& out, u8 attribute);

	public:
		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedWord(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedWord(offset)); }

		u32 readUnsignedWord(Address offset) override;

		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }

		void writeWord(Address offset, u32 value) override;

	private:
		void resize(u32 width, u32 height);

		// A run of cells starting where the last write left off, so a whole grid is one trigger
		// and a row is one per row.
		void blockWrite(Address ramAddress, u32 size, std::vector<u8>& plane, u32& cursor);

		void present();

		void presentToTerminal();
	};
}

#pragma once

// The virtual terminal (plan/v2 SPEC 8): the program's standard input, output and error. What the program writes is
// drawn in the GPU's text plane - UTF-8, the controls and the ANSI subset of SPEC 8.2 (ansi_parser.h) - and what is
// typed in the window reaches it through the line discipline of SPEC 8.3 (line_discipline.h). The host's terminal is
// never involved: a host that wants the bytes (a transcript, a debugger's console) installs a sink and gets a copy.
//
// Without a screen (setScreen), the terminal is only its streams: the bytes still go to the sinks and the input
// still works, but nothing is drawn.

#include <ceres/devices/terminal/ansi_parser.h>
#include <ceres/devices/terminal/line_discipline.h>
#include <ceres/vm/mmio_bus.h>

#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ceres::devices
{
	using namespace vm;

	class GpuDevice;

	class TerminalDevice final : public IODevice
	{
	public:
		static inline constexpr Address StatusRegister = Address(0x00);        // R: b0 input, b1 ready for output, b2 end of input, b3 Ctrl+C pending
		static inline constexpr Address OutputRegister = Address(0x04);        // W: the low byte to the output (UTF-8 and ANSI)
		static inline constexpr Address InputRegister = Address(0x08);         // R: the next input byte (0 if none)
		static inline constexpr Address AvailableRegister = Address(0x0C);     // R: input bytes waiting
		static inline constexpr Address ModeRegister = Address(0x10);          // RW: b0 raw, b1 echo, b2 history, b3 interrupt 19 on input
		static inline constexpr Address ErrorOutputRegister = Address(0x14);   // W: the low byte to the output, in the error colour
		static inline constexpr Address ControlRegister = Address(0x18);       // RW: b0 on, b1 cursor visible, b2 scrollback, b3 autoscroll
		static inline constexpr Address ColsRegister = Address(0x1C);          // R
		static inline constexpr Address RowsRegister = Address(0x20);          // R
		static inline constexpr Address CursorXRegister = Address(0x24);       // RW
		static inline constexpr Address CursorYRegister = Address(0x28);       // RW
		static inline constexpr Address InterruptAckRegister = Address(0x2C);  // W: 1 clears the pending Ctrl+C
		static inline constexpr Address BlockAddressRegister = Address(0xF0);  // W
		static inline constexpr Address BlockLengthRegister = Address(0xF4);   // W
		static inline constexpr Address BlockCommandRegister = Address(0xF8);  // W: 1 write the block, 2 read input into it, 3 write it as error
		static inline constexpr Address BlockCountRegister = Address(0xFC);    // R: bytes the last block moved

		static inline constexpr u32 StatusInputAvailable = 1u << 0;
		static inline constexpr u32 StatusOutputReady = 1u << 1;
		static inline constexpr u32 StatusEndOfInput = 1u << 2;
		static inline constexpr u32 StatusInterrupt = 1u << 3;

		static inline constexpr u32 ModeRaw = 1u << 0;
		static inline constexpr u32 ModeEcho = 1u << 1;
		static inline constexpr u32 ModeHistory = 1u << 2;
		static inline constexpr u32 ModeInterrupt = 1u << 3;
		static inline constexpr u32 DefaultMode = ModeEcho | ModeHistory | ModeInterrupt;

		static inline constexpr u32 ControlOn = 1u << 0;
		static inline constexpr u32 ControlCursor = 1u << 1;
		static inline constexpr u32 ControlScrollback = 1u << 2;
		static inline constexpr u32 ControlAutoscroll = 1u << 3;
		static inline constexpr u32 DefaultControl = ControlOn | ControlCursor | ControlScrollback | ControlAutoscroll;

		static inline constexpr u32 BlockCommandWrite = 1;
		static inline constexpr u32 BlockCommandRead = 2;
		static inline constexpr u32 BlockCommandWriteError = 3;

		// 19 (plan/v2 SPEC 5.6): input came (with Mode bit 3), or Ctrl+C was pressed.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt3;

		// The input waiting for the program; what does not fit is dropped (droppedInputBytes).
		static inline constexpr usize InputBufferCapacity = 8192;

		// The colours of the default cell and of the error stream (the palette's first 16).
		static inline constexpr u32 DefaultInk = 7;
		static inline constexpr u32 DefaultBackground = 0;
		static inline constexpr u32 ErrorInk = 9;

		// A copy of a stream, byte by byte, as the program wrote it.
		using OutputSink = std::function<void(u8)>;

		// The input as a debugger's snapshot keeps it.
		struct State
		{
			std::vector<u8> input;
			bool closed = false;
			bool endOfInput = false;
			bool interrupt = false;
		};

	private:
		// What a stream draws with: its colours and SGR state. Output and error have one each, and one parser each,
		// so a sequence cut in two on one stream is not finished by the other.
		struct Pen
		{
			u32 ink = DefaultInk;
			u32 background = DefaultBackground;
			bool bold = false;
			bool reverse = false;
			u32 baseInk = DefaultInk;   // what SGR 0 and 39 go back to
		};
		class Drawer;

		// The screen.
		GpuDevice* _gpu = nullptr;
		u32 _x = 0, _y = 0;
		bool _pendingWrap = false;
		u32 _savedX = 0, _savedY = 0;
		u32 _regionTop = 0;
		u32 _regionBottom = ~0u;   // ~0: the last row
		Pen _outPen;
		Pen _errPen{ ErrorInk, DefaultBackground, false, false, ErrorInk };
		term::AnsiParser _outParser;
		term::AnsiParser _errParser;
		term::AnsiParser _echoParser;
		u32 _control = DefaultControl;

		// The input.
		mutable std::mutex _inputMutex;
		std::deque<u8> _input;
		u64 _dropped = 0;
		bool _closed = false;        // the host has nothing more to send, ever
		bool _endOfInput = false;    // Ctrl+D: the program is told once its input is drained
		bool _interrupt = false;     // Ctrl+C pending
		u32 _mode = DefaultMode;
		term::LineDiscipline _discipline;
		// Text typed by a script (type()): it goes through the line discipline as the program reads, a key at a
		// time, under whatever mode the program has set by then - so a menu that switches to raw after it starts
		// still gets its arrows, and a line is edited only when it is read. A keystroke from the window waits
		// behind it, in order. The input closes after it when it was asked to close meanwhile.
		std::deque<u32> _typeahead;
		// In _typeahead: a byte a script typed that is not UTF-8, kept as the byte.
		static inline constexpr u32 RawByte = 1u << 29;
		bool _closeAfterTypeahead = false;

		OutputSink _outputSink;
		OutputSink _errorSink;
		u32 _blockAddress = 0;
		u32 _blockLength = 0;
		u32 _blockCount = 0;

	public:
		TerminalDevice() = default;
		TerminalDevice(const TerminalDevice&) = delete;
		TerminalDevice(TerminalDevice&&) = delete;
		~TerminalDevice() override = default;

		TerminalDevice& operator=(const TerminalDevice&) = delete;
		TerminalDevice& operator=(TerminalDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus) { bus.attach(default_mmio::Terminal, *this); }
		void detachFrom(MmioBus& bus) { bus.detach(default_mmio::Terminal); }

		// Where the terminal draws: the GPU's text plane. Null, and nothing is drawn.
		void setScreen(GpuDevice* gpu) noexcept;

		// A copy of every byte the program writes to its output, or to its error stream.
		void setOutputSink(OutputSink sink) { _outputSink = std::move(sink); }
		void setErrorSink(OutputSink sink) { _errorSink = std::move(sink); }
		void clearOutputSink() { _outputSink = nullptr; }

		// A keystroke typed in the window (keyboard.h's): Shift+PageUp and Shift+PageDown move through the scrollback;
		// everything else goes through the line discipline.
		void typeKeystroke(u32 keystroke);
		// Text typed, a character at a time, as if at the keyboard: '\n', '\r' or "\r\n" is Enter, 0x03 Ctrl+C, 0x04 Ctrl+D
		// (--type).
		// It is taken as the program reads (see _typeahead).
		void type(std::string_view utf8);
		// Bytes straight into the program's input, past the line discipline, as from a pipe (a debugger's console).
		void pushInput(std::span<const u8> input);
		void pushInput(std::string_view input) { pushInput(std::span<const u8>(reinterpret_cast<const u8*>(input.data()), input.size())); }
		void pushInput(char input) { pushInput(std::string_view(&input, 1)); }
		// The input is over for good: what is being edited is handed over, and once the program has read everything
		// it is told the input ended. The interrupt is raised so a program halted waiting for input wakes to see it.
		void closeInput();

		bool isInputClosed() const noexcept;
		usize availableBytes() const noexcept;
		// How many more bytes of typing the input can take now: the host feeds scripted input no faster.
		usize inputRoom() const noexcept;
		u64 droppedInputBytes() const noexcept;

		// Paints the report of an exception nobody handled over the screen, in the error colour (plan/v2 F5.6). It is
		// the host's, not the program's: it goes to no sink.
		void showFault(std::string_view text);

		State captureState() const;
		void restoreState(const State& state);

		// A reset: the input, the modes and the pens go back to the start; the screen is the GPU's, which resets too.
		void reset() override;

		u32 read(Address offset) override;
		void write(Address offset, u32 value) override;
		const RegisterMap& registers() const override;

	private:
		friend class Drawer;
		void writeByte(u8 byte, bool error);
		void blockRead(Address ramAddress, u32 size);
		void blockWrite(Address ramAddress, u32 size, bool error);
		void deliver(std::string_view bytes);
		// A keystroke through the line discipline, now.
		void processKeystroke(u32 keystroke);
		// The program looks at its input: typed text goes in until there is something to read, or none is left.
		void refill();
		void finishClose();
		void applyMode() noexcept;
		void syncCursor() noexcept;
	};
}

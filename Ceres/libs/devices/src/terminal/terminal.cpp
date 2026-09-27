#include <ceres/devices/terminal/terminal.h>
#include <ceres/devices/terminal/scrollback.h>
#include <ceres/devices/input/keyboard.h>
#include <ceres/devices/video/gpu.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace ceres::devices
{
	namespace
	{
		// Every register of the device (plan/v2 SPEC 8.1), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x00, "Status",       RegisterAccess::Read,      0x2, false, "Bit 0 input waiting, 1 ready for output, 2 end of input (Ctrl+D), 3 Ctrl+C pending." },
			{ 0x04, "Output",       RegisterAccess::Write,     0x0, false, "The low byte to the output: UTF-8, controls and ANSI sequences." },
			{ 0x08, "Input",        RegisterAccess::Read,      0x0, true,  "The next input byte, or 0 when there is none." },
			{ 0x0C, "Available",    RegisterAccess::Read,      0x0, false, "Input bytes waiting." },
			{ 0x10, "Mode",         RegisterAccess::ReadWrite, TerminalDevice::DefaultMode, false, "Bit 0 raw, 1 echo, 2 history, 3 interrupt 19 when input comes." },
			{ 0x14, "ErrorOutput",  RegisterAccess::Write,     0x0, false, "The low byte to the output, in the error colour." },
			{ 0x18, "Control",      RegisterAccess::ReadWrite, TerminalDevice::DefaultControl, false, "Bit 0 on, 1 cursor visible, 2 scrollback, 3 autoscroll." },
			{ 0x1C, "Cols",         RegisterAccess::Read,      0x0, false, "Columns of the screen." },
			{ 0x20, "Rows",         RegisterAccess::Read,      0x0, false, "Rows of the screen." },
			{ 0x24, "CursorX",      RegisterAccess::ReadWrite, 0x0, false, "The cursor's column." },
			{ 0x28, "CursorY",      RegisterAccess::ReadWrite, 0x0, false, "The cursor's row." },
			{ 0x2C, "InterruptAck", RegisterAccess::Write,     0x0, false, "1 clears the pending Ctrl+C." },
			{ 0xF0, "BlockAddress", RegisterAccess::Write,     0x0, false, "RAM address of a block transfer." },
			{ 0xF4, "BlockLength",  RegisterAccess::Write,     0x0, false, "Bytes in a block transfer." },
			{ 0xF8, "BlockCommand", RegisterAccess::Write,     0x0, false, "1 writes the block to the output, 2 reads input into it, 3 writes it as error." },
			{ 0xFC, "BlockCount",   RegisterAccess::Read,      0x0, false, "Bytes the last block moved." },
		};

		// The palette's first 16 as RGB, for turning a 256-colour index into the nearest of them on 16-bit cells.
		constexpr std::array<u32, 16> Ansi = {
			0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
			0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF };

		u32 nearestOf16(u32 index)
		{
			if (index < 16)
				return index;
			const u32 rgb = video::TextPlane::defaultPaletteEntry(index);
			u32 best = 0;
			u64 bestDistance = ~u64{ 0 };
			for (u32 i = 0; i < 16; ++i)
			{
				u64 distance = 0;
				for (u32 shift = 0; shift <= 16; shift += 8)
				{
					const i64 d = static_cast<i64>((rgb >> shift) & 255) - static_cast<i64>((Ansi[i] >> shift) & 255);
					distance += static_cast<u64>(d * d);
				}
				if (distance < bestDistance)
				{
					bestDistance = distance;
					best = i;
				}
			}
			return best;
		}

		u32 parameter(std::span<const u32> parameters, usize index, u32 fallback)
		{
			return index < parameters.size() && parameters[index] != 0 ? parameters[index] : fallback;
		}
	}

	// Carries out what the parser found, with one pen, on the terminal's screen.
	class TerminalDevice::Drawer final : public term::AnsiParser::Handler
	{
	private:
		TerminalDevice& _t;
		Pen& _pen;

		bool drawing() const noexcept { return _t._gpu != nullptr && (_t._control & ControlOn) != 0; }
		video::TextPlane& plane() const noexcept { return _t._gpu->textPlane(); }
		u32 cols() const noexcept { return _t._gpu ? plane().cols() : 80u; }
		u32 rows() const noexcept { return _t._gpu ? plane().rows() : 25u; }
		u32 bottom() const noexcept { return std::min(_t._regionBottom, rows() - 1); }
		u32 top() const noexcept { return std::min(_t._regionTop, bottom()); }

		u32 cell(u32 glyph) const noexcept
		{
			const bool wide = plane().cellFormat() == 1;
			u32 ink = _pen.ink;
			u32 background = _pen.background;
			if (_pen.bold && ink < 8)
				ink += 8;
			if (_pen.reverse)
				std::swap(ink, background);
			if (wide)
				return glyph | (ink << 8) | (background << 16);
			return glyph | (nearestOf16(ink) << 8) | (nearestOf16(background) << 12);
		}

		void put(u32 x, u32 y, u32 value) noexcept
		{
			const video::TextPlane& p = plane();
			const u32 address = p.cellAddress(x, y);
			if (!_t.vram().backs(address, p.cellBytes()))
				return;
			if (p.cellBytes() == 4)
				_t.vram().write<u32>(address - Vram::BaseValue, value);
			else
				_t.vram().write<u16>(address - Vram::BaseValue, static_cast<u16>(value));
		}

		// Blanks columns [from, to) of row y with the pen's background.
		void blank(u32 y, u32 from, u32 to) noexcept
		{
			if (!drawing())
				return;
			const u32 value = cell(' ');
			for (u32 x = from; x < to && x < cols(); ++x)
				put(x, y, value);
		}

		// The rows of [first, last] move up one; the top one goes to the scrollback if it is the screen's.
		void scrollUp(u32 first, u32 last) noexcept
		{
			if (!drawing())
				return;
			video::TextPlane& p = plane();
			const u32 rowBytes = p.cols() * p.cellBytes();
			if (first == 0 && last == rows() - 1 && (_t._control & ControlScrollback) != 0)
				term::Scrollback::push(p, _t.vram(), p.cellAddress(0, 0));
			if (last > first)
			{
				const u32 to = p.cellAddress(0, first);
				const u32 bytes = (last - first) * rowBytes;
				if (_t.vram().backs(to, bytes + rowBytes))
					std::memmove(_t.vram().span(to - Vram::BaseValue, bytes), _t.vram().data() + (to + rowBytes - Vram::BaseValue), bytes);
			}
			blank(last, 0, cols());
		}

		void lineFeed() noexcept
		{
			if (_t._y == bottom())
			{
				if ((_t._control & ControlAutoscroll) != 0)
					scrollUp(top(), bottom());
			}
			else if (_t._y + 1 < rows())
				++_t._y;
		}

		void sgr(std::span<const u32> parameters)
		{
			if (parameters.empty())
			{
				_pen = Pen{ _pen.baseInk, DefaultBackground, false, false, _pen.baseInk };
				return;
			}
			for (usize i = 0; i < parameters.size(); ++i)
			{
				const u32 p = parameters[i];
				if (p == 0) _pen = Pen{ _pen.baseInk, DefaultBackground, false, false, _pen.baseInk };
				else if (p == 1) _pen.bold = true;
				else if (p == 22) _pen.bold = false;
				else if (p == 7) _pen.reverse = true;
				else if (p == 27) _pen.reverse = false;
				else if (p >= 30 && p <= 37) _pen.ink = p - 30;
				else if (p == 39) _pen.ink = _pen.baseInk;
				else if (p >= 40 && p <= 47) _pen.background = p - 40;
				else if (p == 49) _pen.background = DefaultBackground;
				else if (p >= 90 && p <= 97) _pen.ink = p - 90 + 8;
				else if (p >= 100 && p <= 107) _pen.background = p - 100 + 8;
				else if ((p == 38 || p == 48) && i + 2 < parameters.size() && parameters[i + 1] == 5)
				{
					(p == 38 ? _pen.ink : _pen.background) = parameters[i + 2] & 0xFF;
					i += 2;
				}
			}
		}

	public:
		Drawer(TerminalDevice& terminal, Pen& pen) : _t(terminal), _pen(pen) {}

		void print(u32 codePoint) override
		{
			if (_t._pendingWrap)
			{
				_t._x = 0;
				lineFeed();
				_t._pendingWrap = false;
			}
			if (drawing())
				put(_t._x, _t._y, cell(video::glyphFor(codePoint)));
			if (_t._x + 1 < cols())
				++_t._x;
			else
				_t._pendingWrap = true;
		}

		void control(u8 code) override
		{
			switch (code)
			{
				case '\r': _t._x = 0; break;
				case '\n': _t._x = 0; lineFeed(); break;
				case '\b': if (_t._x > 0) --_t._x; break;
				case '\t': _t._x = std::min(cols() - 1, (_t._x / 8 + 1) * 8); break;
				default: return;   // \a has no sound to make until the audio's tone (F9); the rest mean nothing here
			}
			_t._pendingWrap = false;
		}

		void csi(char final, std::span<const u32> p, char privateMarker) override
		{
			if (privateMarker == '?')
			{
				if ((final == 'h' || final == 'l') && !p.empty() && p[0] == 25)
					_t._control = final == 'h' ? (_t._control | ControlCursor) : (_t._control & ~ControlCursor);
				return;
			}
			if (privateMarker != 0)
				return;
			const u32 n = parameter(p, 0, 1);
			switch (final)
			{
				case 'A': _t._y = _t._y > n ? _t._y - n : 0; break;
				case 'B': _t._y = std::min(rows() - 1, _t._y + n); break;
				case 'C': _t._x = std::min(cols() - 1, _t._x + n); break;
				case 'D': _t._x = _t._x > n ? _t._x - n : 0; break;
				case 'H':
				case 'f':
					_t._y = std::min(rows() - 1, parameter(p, 0, 1) - 1);
					_t._x = std::min(cols() - 1, parameter(p, 1, 1) - 1);
					break;
				case 'J':
				{
					const u32 how = p.empty() ? 0 : p[0];
					if (how == 0)
					{
						blank(_t._y, _t._x, cols());
						for (u32 y = _t._y + 1; y < rows(); ++y) blank(y, 0, cols());
					}
					else if (how == 1)
					{
						for (u32 y = 0; y < _t._y; ++y) blank(y, 0, cols());
						blank(_t._y, 0, _t._x + 1);
					}
					else if (how == 2)
						for (u32 y = 0; y < rows(); ++y) blank(y, 0, cols());
					break;
				}
				case 'K':
				{
					const u32 how = p.empty() ? 0 : p[0];
					if (how == 0) blank(_t._y, _t._x, cols());
					else if (how == 1) blank(_t._y, 0, _t._x + 1);
					else if (how == 2) blank(_t._y, 0, cols());
					break;
				}
				case 'm': sgr(p); break;
				case 's': _t._savedX = _t._x; _t._savedY = _t._y; break;
				case 'u': _t._x = std::min(_t._savedX, cols() - 1); _t._y = std::min(_t._savedY, rows() - 1); break;
				case 'r':
				{
					const u32 first = parameter(p, 0, 1) - 1;
					const u32 last = parameter(p, 1, rows()) - 1;
					if (first < last && last < rows())
					{
						_t._regionTop = first;
						_t._regionBottom = last == rows() - 1 ? ~0u : last;
					}
					else
					{
						_t._regionTop = 0;
						_t._regionBottom = ~0u;
					}
					_t._x = 0;
					_t._y = 0;
					break;
				}
				default: return;   // any other sequence is consumed and does nothing (SPEC 8.2)
			}
			_t._pendingWrap = false;
		}

		void escape(char final) override
		{
			if (final == '7') { _t._savedX = _t._x; _t._savedY = _t._y; }
			else if (final == '8') { _t._x = std::min(_t._savedX, cols() - 1); _t._y = std::min(_t._savedY, rows() - 1); _t._pendingWrap = false; }
		}
	};

	namespace
	{
		// What the line discipline does, done on the terminal.
		class DisciplineOutput final : public term::LineDiscipline::Output
		{
		public:
			std::function<void(std::string_view)> echoTo;
			std::function<void(std::string_view)> deliverTo;
			std::function<void()> interruptTo;
			std::function<void()> endTo;

			void echo(std::string_view bytes) override { echoTo(bytes); }
			void deliver(std::string_view bytes) override { deliverTo(bytes); }
			void interrupt() override { interruptTo(); }
			void endOfInput() override { endTo(); }
		};
	}

	void TerminalDevice::setScreen(GpuDevice* gpu) noexcept
	{
		_gpu = gpu;
		syncCursor();
	}

	void TerminalDevice::syncCursor() noexcept
	{
		if (_gpu == nullptr)
			return;
		video::TextPlane& plane = _gpu->textPlane();
		_x = std::min(_x, plane.cols() - 1);
		_y = std::min(_y, plane.rows() - 1);
		const bool visible = (_control & (ControlOn | ControlCursor)) == (ControlOn | ControlCursor);
		plane.setCursor(_x, _y, visible ? (video::TextPlane::CursorUnderline | video::TextPlane::CursorBlink) : video::TextPlane::CursorNone);
	}

	void TerminalDevice::writeByte(u8 byte, bool error)
	{
		if (const OutputSink& sink = error ? _errorSink : _outputSink)
			sink(byte);
		if (_gpu != nullptr)
		{
			// Output brings a view scrolled back to the live screen, as a terminal does.
			video::TextPlane& plane = _gpu->textPlane();
			if (plane.scrollY() != 0)
				plane.setScrollY(0);
			_x = std::min(_x, plane.cols() - 1);
			_y = std::min(_y, plane.rows() - 1);
		}
		Drawer drawer{ *this, error ? _errPen : _outPen };
		(error ? _errParser : _outParser).feed(byte, drawer);
		syncCursor();
	}

	void TerminalDevice::showFault(std::string_view text)
	{
		if (_gpu == nullptr)
			return;
		Pen pen{ 15, 1, false, false, 15 };   // bright white on red
		Drawer drawer{ *this, pen };
		term::AnsiParser parser;
		if (_x != 0 || _pendingWrap)
			drawer.control('\n');
		_pendingWrap = false;
		for (char c : text)
			parser.feed(static_cast<u8>(c), drawer);
		drawer.control('\n');
		syncCursor();
	}

	void TerminalDevice::deliver(std::string_view bytes)
	{
		bool delivered = false;
		{
			const std::lock_guard lock{ _inputMutex };
			for (char c : bytes)
			{
				if (_input.size() >= InputBufferCapacity)
				{
					++_dropped;
					continue;
				}
				_input.push_back(static_cast<u8>(c));
				delivered = true;
			}
			if (delivered)
				_endOfInput = false;
		}
		if (delivered && (_mode & ModeInterrupt) != 0)
			raiseInterrupt(Interrupt);
	}

	void TerminalDevice::typeKeystroke(u32 keystroke)
	{
		constexpr u32 pageUp = KeyboardDevice::KeyNamed | KeyboardDevice::KeyShift | scancode::PageUp;
		constexpr u32 pageDown = KeyboardDevice::KeyNamed | KeyboardDevice::KeyShift | scancode::PageDown;
		if (_gpu != nullptr && (keystroke == pageUp || keystroke == pageDown))
		{
			if ((_control & ControlScrollback) != 0)
			{
				const i32 page = static_cast<i32>(std::max<u32>(1, _gpu->textPlane().rows() - 1));
				term::Scrollback::scroll(_gpu->textPlane(), keystroke == pageUp ? page : -page);
			}
			return;
		}
		if (!_typeahead.empty())
		{
			_typeahead.push_back(keystroke);   // behind what a script typed, in order
			return;
		}
		processKeystroke(keystroke);
	}

	void TerminalDevice::processKeystroke(u32 keystroke)
	{
		if (_gpu != nullptr && _gpu->textPlane().scrollY() != 0)
			_gpu->textPlane().setScrollY(0);   // typing brings the view back to the live screen

		DisciplineOutput out;
		out.echoTo = [this](std::string_view bytes)
		{
			Drawer drawer{ *this, _outPen };
			for (char c : bytes)
				_echoParser.feed(static_cast<u8>(c), drawer);
			syncCursor();
		};
		out.deliverTo = [this](std::string_view bytes) { deliver(bytes); };
		out.interruptTo = [this]
		{
			{
				const std::lock_guard lock{ _inputMutex };
				_interrupt = true;
			}
			raiseInterrupt(Interrupt);
		};
		out.endTo = [this]
		{
			{
				const std::lock_guard lock{ _inputMutex };
				_endOfInput = true;
			}
			raiseInterrupt(Interrupt);
		};
		_discipline.key(keystroke & ~KeyboardDevice::KeyShift, out);
	}

	void TerminalDevice::type(std::string_view utf8)
	{
		for (usize i = 0; i < utf8.size();)
		{
			const u8 lead = static_cast<u8>(utf8[i]);
			u32 length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
			u32 cp = length == 1 ? lead : lead & (0x7Fu >> length);
			if (i + length > utf8.size())
				length = 1;
			for (u32 k = 1; k < length; ++k)
				cp = (cp << 6) | (static_cast<u8>(utf8[i + k]) & 0x3Fu);
			i += length;
			_typeahead.push_back(cp == '\r' ? u32{ '\n' } : cp);
		}
	}

	void TerminalDevice::refill()
	{
		while (!_typeahead.empty())
		{
			{
				const std::lock_guard lock{ _inputMutex };
				if (!_input.empty())
					return;
			}
			const u32 keystroke = _typeahead.front();
			_typeahead.pop_front();
			processKeystroke(keystroke);
		}
		if (_closeAfterTypeahead)
		{
			_closeAfterTypeahead = false;
			finishClose();
		}
	}

	void TerminalDevice::pushInput(std::span<const u8> input)
	{
		deliver(std::string_view(reinterpret_cast<const char*>(input.data()), input.size()));
	}

	void TerminalDevice::closeInput()
	{
		if (!_typeahead.empty())
		{
			_closeAfterTypeahead = true;   // once the program has read what the script typed
			return;
		}
		finishClose();
	}

	void TerminalDevice::finishClose()
	{
		DisciplineOutput out;
		out.echoTo = [](std::string_view) {};
		out.deliverTo = [this](std::string_view bytes) { deliver(bytes); };
		out.interruptTo = [] {};
		out.endTo = [] {};
		_discipline.flush(out);
		{
			const std::lock_guard lock{ _inputMutex };
			_closed = true;
		}
		raiseInterrupt(Interrupt);
	}

	bool TerminalDevice::isInputClosed() const noexcept
	{
		const std::lock_guard lock{ _inputMutex };
		return _closed;
	}

	usize TerminalDevice::availableBytes() const noexcept
	{
		const std::lock_guard lock{ _inputMutex };
		return _input.size();
	}

	usize TerminalDevice::inputRoom() const noexcept
	{
		const std::lock_guard lock{ _inputMutex };
		const usize used = _input.size() + _discipline.pendingBytes() + _typeahead.size();
		return used < InputBufferCapacity ? InputBufferCapacity - used : 0;
	}

	u64 TerminalDevice::droppedInputBytes() const noexcept
	{
		const std::lock_guard lock{ _inputMutex };
		return _dropped;
	}

	TerminalDevice::State TerminalDevice::captureState() const
	{
		const std::lock_guard lock{ _inputMutex };
		return State{ std::vector<u8>(_input.begin(), _input.end()), _closed, _endOfInput, _interrupt };
	}

	void TerminalDevice::restoreState(const State& state)
	{
		const std::lock_guard lock{ _inputMutex };
		_input.assign(state.input.begin(), state.input.end());
		_closed = state.closed;
		_endOfInput = state.endOfInput;
		_interrupt = state.interrupt;
	}

	void TerminalDevice::applyMode() noexcept
	{
		_discipline.setRaw((_mode & ModeRaw) != 0);
		_discipline.setEcho((_mode & ModeEcho) != 0);
		_discipline.setHistory((_mode & ModeHistory) != 0);
	}

	void TerminalDevice::reset()
	{
		{
			const std::lock_guard lock{ _inputMutex };
			_endOfInput = false;
			_interrupt = false;
		}
		_mode = DefaultMode;
		_control = DefaultControl;
		applyMode();
		_discipline.reset();
		_outPen = Pen{};
		_errPen = Pen{ ErrorInk, DefaultBackground, false, false, ErrorInk };
		_outParser.reset();
		_errParser.reset();
		_echoParser.reset();
		_x = _y = _savedX = _savedY = 0;
		_pendingWrap = false;
		_regionTop = 0;
		_regionBottom = ~0u;
		_blockCount = 0;
		syncCursor();
	}

	void TerminalDevice::blockRead(Address ramAddress, u32 size)
	{
		refill();
		_blockCount = 0;
		if (size == 0)
			return;
		const u32 clampSize = memory().clampBlockSize(ramAddress, size);
		if (clampSize == 0)
			return;
		auto buffer = memory().peekMutBytes(ramAddress, clampSize);
		const std::lock_guard lock{ _inputMutex };
		usize count = 0;
		while (count < buffer.size() && !_input.empty())
		{
			buffer[count++] = _input.front();
			_input.pop_front();
		}
		_blockCount = static_cast<u32>(count);
	}

	void TerminalDevice::blockWrite(Address ramAddress, u32 size, bool error)
	{
		_blockCount = 0;
		if (size == 0)
			return;
		const u32 clampSize = memory().clampBlockSize(ramAddress, size);
		if (clampSize == 0)
			return;
		const auto buffer = memory().peekBytes(ramAddress, clampSize);
		for (u8 byte : buffer)
			writeByte(byte, error);
		_blockCount = clampSize;
	}

	u32 TerminalDevice::read(Address offset)
	{
		if (offset == StatusRegister || offset == InputRegister || offset == AvailableRegister)
			refill();
		switch (offset.value())
		{
			case StatusRegister.value():
			{
				const std::lock_guard lock{ _inputMutex };
				u32 status = StatusOutputReady;
				if (!_input.empty())
					status |= StatusInputAvailable;
				else if (_closed || _endOfInput)
					status |= StatusEndOfInput;
				if (_interrupt)
					status |= StatusInterrupt;
				return status;
			}
			case InputRegister.value():
			{
				const std::lock_guard lock{ _inputMutex };
				if (_input.empty())
					return 0;
				const u8 byte = _input.front();
				_input.pop_front();
				return byte;
			}
			case AvailableRegister.value(): return static_cast<u32>(availableBytes());
			case ModeRegister.value(): return _mode;
			case ControlRegister.value(): return _control;
			case ColsRegister.value(): return _gpu ? _gpu->textPlane().cols() : 0u;
			case RowsRegister.value(): return _gpu ? _gpu->textPlane().rows() : 0u;
			case CursorXRegister.value(): return _x;
			case CursorYRegister.value(): return _y;
			case BlockCountRegister.value(): return _blockCount;
			default: return 0;
		}
	}

	void TerminalDevice::write(Address offset, u32 value)
	{
		switch (offset.value())
		{
			case OutputRegister.value(): writeByte(static_cast<u8>(value), false); break;
			case ErrorOutputRegister.value(): writeByte(static_cast<u8>(value), true); break;
			case ModeRegister.value(): _mode = value & (ModeRaw | ModeEcho | ModeHistory | ModeInterrupt); applyMode(); break;
			case ControlRegister.value(): _control = value & (ControlOn | ControlCursor | ControlScrollback | ControlAutoscroll); syncCursor(); break;
			case CursorXRegister.value(): _x = value; _pendingWrap = false; syncCursor(); break;
			case CursorYRegister.value(): _y = value; _pendingWrap = false; syncCursor(); break;
			case InterruptAckRegister.value():
				if ((value & 1u) != 0)
				{
					const std::lock_guard lock{ _inputMutex };
					_interrupt = false;
				}
				break;
			case BlockAddressRegister.value(): _blockAddress = value; break;
			case BlockLengthRegister.value(): _blockLength = value; break;
			case BlockCommandRegister.value():
				if (value == BlockCommandWrite)
					blockWrite(Address(_blockAddress), _blockLength, false);
				else if (value == BlockCommandRead)
					blockRead(Address(_blockAddress), _blockLength);
				else if (value == BlockCommandWriteError)
					blockWrite(Address(_blockAddress), _blockLength, true);
				break;
			default: break;
		}
	}

	const RegisterMap& TerminalDevice::registers() const
	{
		static constexpr RegisterMap map{ "terminal", Registers };
		return map;
	}
}

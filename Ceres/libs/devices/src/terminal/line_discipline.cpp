#include <ceres/devices/terminal/line_discipline.h>
#include <ceres/devices/input/keyboard.h>

#include <format>

namespace ceres::devices::term
{
	namespace
	{
		constexpr u32 CtrlC = 0x03;
		constexpr u32 CtrlD = 0x04;
		constexpr u32 CtrlU = 0x15;

		std::string left(usize n) { return n == 0 ? std::string{} : std::format("\x1b[{}D", n); }
		std::string right(usize n) { return n == 0 ? std::string{} : std::format("\x1b[{}C", n); }

		std::string utf8(const std::vector<u32>& text, usize from = 0)
		{
			std::string out;
			for (usize i = from; i < text.size(); ++i)
				appendUtf8(out, text[i]);
			return out;
		}
	}

	void appendUtf8(std::string& out, u32 cp)
	{
		if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
			cp = 0xFFFD;
		if (cp < 0x80)
			out += static_cast<char>(cp);
		else if (cp < 0x800)
		{
			out += static_cast<char>(0xC0 | (cp >> 6));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else if (cp < 0x10000)
		{
			out += static_cast<char>(0xE0 | (cp >> 12));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
		else
		{
			out += static_cast<char>(0xF0 | (cp >> 18));
			out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}

	void LineDiscipline::reset() noexcept
	{
		_afterCr = false;
		_line.clear();
		_cursor = 0;
		_lines.clear();
		_recall = 0;
		_draft.clear();
	}

	usize LineDiscipline::pendingBytes() const noexcept
	{
		usize bytes = 0;
		for (u32 cp : _line)
			bytes += cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
		return bytes;
	}

	// Shows `with` in place of the line: back to its start, what it says, the rest of the old line cleared.
	void LineDiscipline::replaceLine(const std::vector<u32>& with, Output& out)
	{
		if (_echo)
			out.echo(left(_cursor) + "\x1b[K" + utf8(with));
		_line = with;
		_cursor = _line.size();
	}

	void LineDiscipline::submit(Output& out, bool newline)
	{
		std::string bytes = utf8(_line);
		if (newline)
			bytes += '\n';
		if (_history && !_line.empty() && (_lines.empty() || _lines.back() != _line))
		{
			_lines.push_back(_line);
			if (_lines.size() > HistoryLines)
				_lines.pop_front();
		}
		_line.clear();
		_cursor = 0;
		_recall = 0;
		_draft.clear();
		out.deliver(bytes);
	}

	void LineDiscipline::flush(Output& out)
	{
		if (!_line.empty())
			submit(out, false);
	}

	void LineDiscipline::key(u32 keystroke, Output& out)
	{
		const bool named = (keystroke & KeyboardDevice::KeyNamed) != 0;
		const u32 code = keystroke & ~KeyboardDevice::KeyNamed;

		if (_raw)
		{
			if (named)
				out.deliver(keystrokeToTerminalBytes(keystroke));
			else
			{
				std::string bytes;
				appendUtf8(bytes, code);
				out.deliver(bytes);
			}
			return;
		}

		if (named)
		{
			switch (code)
			{
				case scancode::Return:
				case scancode::KeypadEnter:
					if (_echo)
						out.echo(right(_line.size() - _cursor) + "\n");
					submit(out, true);
					return;
				case scancode::Backspace:
					break;   // below, with the character forms
				case scancode::Tab:
					keystroke = '\t';
					break;
				case scancode::Delete:
					if (_cursor < _line.size())
					{
						_line.erase(_line.begin() + static_cast<std::ptrdiff_t>(_cursor));
						if (_echo)
							out.echo(utf8(_line, _cursor) + " " + left(_line.size() - _cursor + 1));
					}
					return;
				case scancode::Left:
					if (_cursor > 0)
					{
						--_cursor;
						if (_echo) out.echo(left(1));
					}
					return;
				case scancode::Right:
					if (_cursor < _line.size())
					{
						++_cursor;
						if (_echo) out.echo(right(1));
					}
					return;
				case scancode::Home:
					if (_echo) out.echo(left(_cursor));
					_cursor = 0;
					return;
				case scancode::End:
					if (_echo) out.echo(right(_line.size() - _cursor));
					_cursor = _line.size();
					return;
				case scancode::Up:
					if (_history && _recall < _lines.size())
					{
						if (_recall == 0)
							_draft = _line;
						++_recall;
						replaceLine(_lines[_lines.size() - _recall], out);
					}
					return;
				case scancode::Down:
					if (_history && _recall > 0)
					{
						--_recall;
						replaceLine(_recall == 0 ? _draft : _lines[_lines.size() - _recall], out);
					}
					return;
				default:
					return;   // Esc, Insert, the page and function keys: nothing in a line being edited
			}
		}

		const u32 cp = named ? (code == scancode::Backspace ? 0x08u : keystroke) : keystroke;
		const bool afterCr = _afterCr;
		_afterCr = cp == '\r';
		switch (cp)
		{
			case '\n':
				if (afterCr)
					return;   // "\r\n" is one Enter, as a file's line end
				[[fallthrough]];
			case '\r':
				if (_echo)
					out.echo(right(_line.size() - _cursor) + "\n");
				submit(out, true);
				return;
			case 0x08:
			case 0x7F:
				if (_cursor > 0)
				{
					--_cursor;
					_line.erase(_line.begin() + static_cast<std::ptrdiff_t>(_cursor));
					if (_echo)
						out.echo("\b" + utf8(_line, _cursor) + " " + left(_line.size() - _cursor + 1));
				}
				return;
			case CtrlU:
				replaceLine({}, out);
				return;
			case CtrlC:
				if (_echo)
					out.echo(right(_line.size() - _cursor) + "^C\n");
				_line.clear();
				_cursor = 0;
				_recall = 0;
				out.interrupt();
				return;
			case CtrlD:
				if (_line.empty())
					out.endOfInput();
				else
				{
					if (_echo)
						out.echo(right(_line.size() - _cursor));
					submit(out, false);
				}
				return;
			default:
				break;
		}
		if (cp < 0x20 && cp != '\t')
			return;   // the other control characters mean nothing in a line
		if (pendingBytes() >= MaxLine)
			return;
		_line.insert(_line.begin() + static_cast<std::ptrdiff_t>(_cursor), cp);
		++_cursor;
		if (_echo)
			out.echo(utf8(_line, _cursor - 1) + left(_line.size() - _cursor));
	}
}

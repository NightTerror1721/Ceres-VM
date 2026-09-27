#include <ceres/devices/terminal/ansi_parser.h>

namespace ceres::devices::term
{
	void AnsiParser::feed(u8 byte, Handler& handler)
	{
		switch (_state)
		{
			case State::Ground:
			{
				if (_needed != 0)
				{
					if ((byte & 0xC0) == 0x80)
					{
						_codePoint = (_codePoint << 6) | (byte & 0x3Fu);
						if (--_needed == 0)
						{
							// Surrogates and what is past U+10FFFF are not characters.
							const bool valid = _codePoint <= 0x10FFFF && !(_codePoint >= 0xD800 && _codePoint <= 0xDFFF);
							handler.print(valid ? _codePoint : 0xFFFD);
						}
						return;
					}
					// The sequence broke off: what it had is one bad character, and this byte starts afresh.
					_needed = 0;
					handler.print(0xFFFD);
				}
				if (byte == 0x1B)
				{
					_state = State::Escape;
					return;
				}
				if (byte < 0x20)
				{
					handler.control(byte);
					return;
				}
				if (byte == 0x7F)
					return;
				if (byte < 0x80)
				{
					handler.print(byte);
					return;
				}
				if (byte >= 0xC2 && byte <= 0xDF) { _codePoint = byte & 0x1Fu; _needed = 1; return; }
				if (byte >= 0xE0 && byte <= 0xEF) { _codePoint = byte & 0x0Fu; _needed = 2; return; }
				if (byte >= 0xF0 && byte <= 0xF4) { _codePoint = byte & 0x07u; _needed = 3; return; }
				handler.print(0xFFFD);   // a continuation byte with no lead, or a lead UTF-8 does not use
				return;
			}

			case State::Escape:
				if (byte == '[')
				{
					_state = State::Csi;
					_parameters.fill(0);
					_count = 0;
					_digits = false;
					_private = 0;
				}
				else if (byte == ']')
					_state = State::Osc;
				else if (byte >= 0x20 && byte < 0x30)
					return;   // an intermediate byte (ESC ( B and the like): wait for the final one
				else
				{
					_state = State::Ground;
					if (byte >= 0x30 && byte < 0x7F)
						handler.escape(static_cast<char>(byte));
				}
				return;

			case State::Csi:
			case State::CsiIgnore:
				if (byte >= '0' && byte <= '9')
				{
					if (_count < MaxParameters)
					{
						const u32 value = _parameters[_count] * 10 + (byte - '0');
						_parameters[_count] = value > 0xFFFF ? 0xFFFF : value;
					}
					_digits = true;
				}
				else if (byte == ';' || byte == ':')
				{
					if (_count < MaxParameters)
						++_count;
					_digits = false;
				}
				else if (byte >= '<' && byte <= '?')
				{
					if (_count == 0 && !_digits && _private == 0)
						_private = static_cast<char>(byte);
					else
						_state = State::CsiIgnore;   // a marker in the middle: not a sequence anyone sends
				}
				else if (byte >= 0x20 && byte < 0x30)
					_state = State::CsiIgnore;       // intermediate bytes: none of the sequences the terminal knows has them
				else if (byte >= 0x40 && byte <= 0x7E)
				{
					const bool known = _state == State::Csi;
					_state = State::Ground;
					if (known)
					{
						const usize used = (_digits || _count > 0) ? (_count < MaxParameters ? _count + 1 : MaxParameters) : 0;
						handler.csi(static_cast<char>(byte), std::span<const u32>(_parameters.data(), used), _private);
					}
				}
				else if (byte == 0x1B)
					_state = State::Escape;          // a sequence cut short by another one
				else if (byte < 0x20)
					handler.control(byte);           // a control inside a sequence still acts, as on a real terminal
				return;

			case State::Osc:
				if (byte == 0x07)
					_state = State::Ground;
				else if (byte == 0x1B)
					_state = State::OscEscape;
				return;

			case State::OscEscape:
				_state = byte == '\\' ? State::Ground : State::Osc;
				return;
		}
	}
}

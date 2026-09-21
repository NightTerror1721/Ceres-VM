#pragma once

// Turns the bytes a terminal sends for the keys a person presses back into keys: a typed character, or a
// named key such as Enter, Escape or an arrow. A terminal has no notion of a key going up, so every key
// comes out as a press followed at once by a release.
//
// Pure: bytes in, actions out, no clock and no console. The host's console reader feeds it and applies the
// actions to the keyboard device; the one thing it cannot know is whether an Escape is a key or the start of
// a sequence, which only a pause after it can say - the reader calls flushPending() when the input goes quiet.

#include <ceres/core/base/types.h>
#include <ceres/devices/input_devices.h>

#include <vector>

namespace ceres::driver
{
	// What a decoded byte turned into: a typed character, or a key event.
	struct KeyAction
	{
		bool isText = false;    // true: value is a Unicode code point; false: value is a scancode
		u32 value = 0;
		bool pressed = true;    // for a key event

		bool operator==(const KeyAction&) const = default;
	};

	// The scancode of an ASCII character's key on a US layout, or 0 for one with no key of its own here.
	// A capital maps to the same key as its small letter: the shift is not reported.
	constexpr u32 scancodeForAscii(u32 c) noexcept
	{
		if (c >= 'a' && c <= 'z') return 4 + (c - 'a');
		if (c >= 'A' && c <= 'Z') return 4 + (c - 'A');
		if (c >= '1' && c <= '9') return 30 + (c - '1');
		if (c == '0') return 39;
		if (c == ' ') return devices::scancode::Space;
		return 0;
	}

	class KeyDecoder
	{
	public:
		// One byte of input. Appends what it completes to `out` (often nothing: it may be the middle of
		// an escape sequence or of a multi-byte character).
		void feed(u8 byte, std::vector<KeyAction>& out)
		{
			switch (_state)
			{
				case State::Escape:
					if (byte == '[') { _state = State::Csi; _params = 0; _paramCount = 0; _haveParam = false; return; }
					if (byte == 'O') { _state = State::Ss3; return; }
					// An Escape followed by anything else is the key, and then that byte on its own.
					_state = State::Ground;
					tap(devices::scancode::Escape, out);
					feed(byte, out);
					return;

				case State::Csi:
					if (byte >= '0' && byte <= '9')
					{
						_params = _params * 10 + (byte - '0');
						_haveParam = true;
						return;
					}
					if (byte == ';')
					{
						if (_paramCount == 0) _first = _haveParam ? _params : 0;
						++_paramCount;
						_params = 0;
						_haveParam = false;
						return;
					}
					if (byte >= 0x40 && byte <= 0x7E)
					{
						const u32 first = _paramCount == 0 ? (_haveParam ? _params : 0) : _first;
						_state = State::Ground;
						csiFinal(static_cast<char>(byte), first, out);
						return;
					}
					_state = State::Ground;   // not a sequence we know: drop it rather than type it
					return;

				case State::Ss3:
					_state = State::Ground;
					ss3Final(static_cast<char>(byte), out);
					return;

				case State::Utf8:
					if ((byte & 0xC0) != 0x80)
					{
						_state = State::Ground;   // a broken character: forget it and read this byte afresh
						feed(byte, out);
						return;
					}
					_codePoint = (_codePoint << 6) | (byte & 0x3Fu);
					if (--_continuation == 0)
					{
						_state = State::Ground;
						text(_codePoint, out);
					}
					return;

				case State::Ground:
					break;
			}

			if (byte == 0x1B) { _state = State::Escape; return; }
			if (byte == '\r' || byte == '\n') { tap(devices::scancode::Return, out); return; }
			if (byte == 0x7F || byte == 0x08) { tap(devices::scancode::Backspace, out); return; }
			if (byte == '\t') { tap(devices::scancode::Tab, out); return; }
			if (byte < 0x20) return;             // another control character types nothing
			if (byte < 0x80) { text(byte, out); return; }
			if (byte >= 0xF0 && byte < 0xF8) { _state = State::Utf8; _continuation = 3; _codePoint = byte & 0x07u; return; }
			if (byte >= 0xE0 && byte < 0xF0) { _state = State::Utf8; _continuation = 2; _codePoint = byte & 0x0Fu; return; }
			if (byte >= 0xC0 && byte < 0xE0) { _state = State::Utf8; _continuation = 1; _codePoint = byte & 0x1Fu; return; }
			// a stray continuation byte: nothing to type
		}

		// True while the last byte could be the start of something longer - an Escape waiting to learn
		// whether a sequence follows.
		bool pending() const noexcept { return _state != State::Ground; }

		// The input went quiet. A lone Escape is the Escape key; half of anything else is abandoned.
		void flushPending(std::vector<KeyAction>& out)
		{
			if (_state == State::Escape)
				tap(devices::scancode::Escape, out);
			_state = State::Ground;
		}

	private:
		enum class State { Ground, Escape, Csi, Ss3, Utf8 };

		static void tap(u32 scancode, std::vector<KeyAction>& out)
		{
			out.push_back({false, scancode, true});
			out.push_back({false, scancode, false});
		}

		static void text(u32 codePoint, std::vector<KeyAction>& out)
		{
			if (codePoint < 0x80)
				if (const u32 key = scancodeForAscii(codePoint); key != 0)
					out.push_back({false, key, true});
			out.push_back({true, codePoint, true});
			if (codePoint < 0x80)
				if (const u32 key = scancodeForAscii(codePoint); key != 0)
					out.push_back({false, key, false});
		}

		static void csiFinal(char final, u32 param, std::vector<KeyAction>& out)
		{
			using namespace devices::scancode;
			switch (final)
			{
				case 'A': tap(Up, out); return;
				case 'B': tap(Down, out); return;
				case 'C': tap(Right, out); return;
				case 'D': tap(Left, out); return;
				case 'H': tap(Home, out); return;
				case 'F': tap(End, out); return;
				case 'P': tap(F1, out); return;
				case 'Q': tap(F1 + 1, out); return;
				case 'R': tap(F1 + 2, out); return;
				case 'S': tap(F1 + 3, out); return;
				case '~':
					switch (param)
					{
						case 1: case 7: tap(Home, out); return;
						case 2: tap(Insert, out); return;
						case 3: tap(Delete, out); return;
						case 4: case 8: tap(End, out); return;
						case 5: tap(PageUp, out); return;
						case 6: tap(PageDown, out); return;
						case 11: case 12: case 13: case 14: tap(F1 + (param - 11), out); return;
						case 15: tap(F1 + 4, out); return;
						case 17: case 18: case 19: case 20: case 21: tap(F1 + 5 + (param - 17), out); return;
						case 23: case 24: tap(F1 + 10 + (param - 23), out); return;
						default: return;
					}
				default: return;
			}
		}

		static void ss3Final(char final, std::vector<KeyAction>& out)
		{
			using namespace devices::scancode;
			switch (final)
			{
				case 'A': tap(Up, out); return;
				case 'B': tap(Down, out); return;
				case 'C': tap(Right, out); return;
				case 'D': tap(Left, out); return;
				case 'H': tap(Home, out); return;
				case 'F': tap(End, out); return;
				case 'P': tap(F1, out); return;
				case 'Q': tap(F1 + 1, out); return;
				case 'R': tap(F1 + 2, out); return;
				case 'S': tap(F1 + 3, out); return;
				default: return;
			}
		}

		State _state = State::Ground;
		u32 _params = 0;
		u32 _first = 0;
		u32 _paramCount = 0;
		bool _haveParam = false;
		u32 _codePoint = 0;
		u32 _continuation = 0;
	};
}

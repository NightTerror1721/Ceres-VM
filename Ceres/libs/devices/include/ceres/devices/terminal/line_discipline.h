#pragma once

// What the terminal does with what is typed (plan/v2 SPEC 8.3), before the program sees it.
//
// Cooked (the default): the line is edited here and handed over whole when Enter is pressed. What is typed is echoed
// (unless echo is off); Backspace and Delete remove a character, Left, Right, Home and End move in the line, Ctrl+U
// empties it, Up and Down walk the history (unless it is off). Ctrl+D on an empty line is the end of the input, and on
// a line with text hands it over without its newline. Ctrl+C drops the line and interrupts the program. The other
// named keys (Esc, Insert, the page keys, the function keys) are the discipline's and do nothing.
//
// Raw: every key goes to the program at once, as the bytes of SPEC 8.3 (D25), Ctrl+C and Ctrl+D included; nothing is
// echoed.
//
// A keystroke is what the keyboard device queues (keyboard.h): a code point - the control characters Ctrl+A to Ctrl+Z
// among them - or KeyNamed | the scancode of a key without a character.

#include <ceres/core/base/types.h>

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace ceres::devices::term
{
	class LineDiscipline
	{
	public:
		// The longest line: what is typed past it is dropped, as on a terminal with a full line buffer.
		static inline constexpr usize MaxLine = 4095;
		static inline constexpr usize HistoryLines = 32;

		class Output
		{
		public:
			virtual ~Output() = default;
			// Bytes to draw on the screen as if written, escape sequences and all (the echo and its corrections).
			virtual void echo(std::string_view bytes) = 0;
			// Bytes for the program's input.
			virtual void deliver(std::string_view bytes) = 0;
			// Ctrl+C in cooked mode.
			virtual void interrupt() = 0;
			// Ctrl+D on an empty line.
			virtual void endOfInput() = 0;
		};

	private:
		bool _raw = false;
		bool _echo = true;
		bool _history = true;
		std::vector<u32> _line;       // code points
		usize _cursor = 0;            // where in _line the next one goes
		std::deque<std::vector<u32>> _lines;   // the history, newest last
		usize _recall = 0;            // how far back Up has gone (0: the line being typed)
		std::vector<u32> _draft;      // the line being typed, kept while the history is shown

		void replaceLine(const std::vector<u32>& with, Output& out);
		void submit(Output& out, bool newline);

	public:
		void setRaw(bool raw) noexcept { _raw = raw; }
		void setEcho(bool echo) noexcept { _echo = echo; }
		void setHistory(bool history) noexcept { _history = history; }
		bool raw() const noexcept { return _raw; }

		void key(u32 keystroke, Output& out);
		// Hands over what is left in the line, without a newline: the input is ending.
		void flush(Output& out);
		// The line being edited, in bytes (UTF-8).
		usize pendingBytes() const noexcept;
		// Back to an empty line and an empty history.
		void reset() noexcept;
	};

	// A code point as UTF-8.
	void appendUtf8(std::string& out, u32 codePoint);
}

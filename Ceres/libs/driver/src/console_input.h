#pragma once

// The host's own console, when the machine's standard input is one: where a person types.
//
// Everywhere else - a pipe, a file, a test's string stream - standard input is just bytes, and
// machine_runner reads it as it always has. A console can give more. Left as it is, it hands a program
// whole lines, and only when Enter is pressed: no key before that, and no arrow at all, because the console's
// own line editor eats them. A program that wants keys as they are pressed asks for raw input (the
// terminal's ModeRegister), and this object takes the console out of line mode for as long as it asks.
//
//   cooked  the console delivers lines; they are passed on as bytes, as they always were
//   raw     the console delivers keys; they are passed on as characters and key events, one by one
//
// One reader thread runs a console for the whole run; setRaw() may be called from any other thread.

#include <ceres/core/base/types.h>

#include <functional>
#include <memory>
#include <span>

namespace ceres::driver
{
	class ConsoleInput
	{
	public:
		// Where what the console delivers goes. All of them run on the reader thread.
		struct Sink
		{
			std::function<void(std::span<const u8>)> bytes;        // cooked: text, a line at a time
			std::function<void(u32 scancode, bool pressed)> key;   // raw: a key went down or up
			std::function<void(u32 codePoint)> text;               // raw: a character was typed
			std::function<void()> endOfInput;                      // the console was closed or sent end-of-file
			std::function<bool()> stopped;                         // true once the machine is done
		};

		// The console standard input is attached to, or null when it is not one (a pipe, a file, none).
		static std::shared_ptr<ConsoleInput> open();

		virtual ~ConsoleInput() = default;

		// Raw or cooked. Safe from any thread; the console is switched by the reader thread itself, so this
		// returns at once.
		virtual void setRaw(bool raw) = 0;

		// Reads until the input ends, restore() is called or sink.stopped() says the machine is done. The
		// reader thread's whole life.
		virtual void run(const Sink& sink) = 0;

		// Puts the console back the way it was found. Must run before the process exits, or the shell that
		// started it is left with a console that does not echo. Safe to call twice.
		virtual void restore() = 0;
	};
}

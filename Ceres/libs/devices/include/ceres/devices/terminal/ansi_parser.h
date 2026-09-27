#pragma once

// The terminal's output decoder (plan/v2 SPEC 8.2): bytes in, as a program writes them, and out come characters
// (UTF-8 decoded), control characters and escape sequences, for the terminal to act on. It knows the grammar, not
// what a sequence means: an ESC [ ... sequence is handed over whole (its parameters, a private marker such as '?',
// its final byte), and the terminal carries out the ones it knows and ignores the rest. An OSC (ESC ]) is consumed
// up to its BEL or ESC \ and dropped. A byte sequence that is not UTF-8 is shown as U+FFFD, one per bad byte.

#include <ceres/core/base/types.h>

#include <array>
#include <span>

namespace ceres::devices::term
{
	class AnsiParser
	{
	public:
		static inline constexpr usize MaxParameters = 16;

		class Handler
		{
		public:
			virtual ~Handler() = default;
			virtual void print(u32 codePoint) = 0;
			// A C0 control: \r, \n, \b, \t, \a, ... (not ESC, which starts a sequence).
			virtual void control(u8 code) = 0;
			// ESC [ <private> <parameters> <final>. A parameter left out is 0; `privateMarker` is 0 or one of < = > ?.
			virtual void csi(char final, std::span<const u32> parameters, char privateMarker) = 0;
			// ESC <final>, for any final byte other than [ and ]: ESC 7, ESC 8, ...
			virtual void escape(char final) = 0;
		};

	private:
		enum class State : u8 { Ground, Escape, Csi, CsiIgnore, Osc, OscEscape };
		State _state = State::Ground;
		std::array<u32, MaxParameters> _parameters{};
		usize _count = 0;
		bool _digits = false;
		char _private = 0;
		// A UTF-8 sequence in progress.
		u32 _codePoint = 0;
		u32 _needed = 0;

	public:
		void feed(u8 byte, Handler& handler);
		void reset() noexcept { *this = AnsiParser{}; }
		// Whether a character or a sequence is half way through.
		bool idle() const noexcept { return _state == State::Ground && _needed == 0; }
	};
}

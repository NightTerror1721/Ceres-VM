#pragma once

// Evaluating an expression against a machine that is standing still.
//
// This is what a watch, a hover, a conditional breakpoint and a logpoint all need, and none of
// them could exist without it: the assembler's own constant folding runs at assembly time and
// cannot even reference an identifier, so there was nothing to reuse.
//
// The language is small on purpose - registers, flags, symbols, typed memory loads, and the
// arithmetic to combine them:
//
//     r3                      a register
//     sp < 0x1000             a comparison, which is what a breakpoint condition is
//     zero                    a flag, as 0 or 1
//     total                   a variable, read through its declared type
//     scores[2]               one element of an array
//     [r1 + 4]                a word loaded from memory
//     u8[r2]                  a byte loaded from memory
//     ticks % 1000 == 0       arithmetic over any of the above

#include <ceres/core/base/types.h>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace ceres::debug
{
	class DebugSession;

	struct EvalResult
	{
		// Always filled: the rendered form, which is what a watch window shows.
		std::string text;
		// Set when the result is a number rather than a string or an array.
		std::optional<i64> integer;
		std::optional<f32> real;
		// Where the value lives, when it came from memory - lets a watch offer the memory viewer.
		std::optional<u32> address;
		std::string type;

		// What a breakpoint condition asks. A string or an array is true when it is non-empty,
		// which matters less than a number being true when it is not zero.
		bool truthy() const noexcept
		{
			if (integer.has_value())
				return integer.value() != 0;
			if (real.has_value())
				return real.value() != 0.0f;
			return !text.empty() && text != "\"\"";
		}
	};

	// Reads the machine but never changes it; the session is const for exactly that reason.
	std::expected<EvalResult, std::string> evaluate(const DebugSession& session, std::string_view expression);

	// Replaces every {expression} in a logpoint message with what it evaluates to. A expression
	// that fails is left in place surrounded by its error rather than aborting the whole message:
	// a logpoint that silently stops logging is worse than one that says what went wrong.
	std::string interpolate(const DebugSession& session, std::string_view message);
}

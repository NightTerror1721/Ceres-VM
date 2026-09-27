#pragma once

// --keys (plan/v2 SPEC 10): keyboard events at instants of the machine's time, for a run without anyone at the
// keyboard. A line a event, '#' starts a comment:
//
//   <ms> down <key>       the key goes down at <ms> milliseconds of machine time
//   <ms> up <key>         and up
//   <ms> press <key>      down, and up a millisecond later
//   <ms> text <text>      the rest of the line typed as text (what a layout would make of the keys)
//
// A key is a scancode in decimal or a name: a-z, 0-9, Return (Enter), Escape (Esc), Backspace, Tab, Space, Up, Down,
// Left, Right, Home, End, PageUp, PageDown, Insert, Delete, F1-F12, LeftCtrl, RightCtrl, LeftShift, RightShift.

#include <ceres/driver/input_journal.h>

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace ceres::driver
{
	// The events of a script, stamped with their cycles at `cpuHz`, in the order they come.
	std::expected<std::vector<InputEvent>, std::string> parseKeyScript(std::string_view text, u64 cpuHz);
}

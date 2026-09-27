#include "key_script.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <utility>

namespace ceres::driver
{
	namespace
	{
		std::string lower(std::string_view text)
		{
			std::string out(text);
			for (char& c : out)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return out;
		}

		std::optional<u32> number(std::string_view text)
		{
			u32 value = 0;
			const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
			if (text.empty() || error != std::errc{} || end != text.data() + text.size())
				return std::nullopt;
			return value;
		}

		// A key by name, as the keyboard reports it (SDL's scancodes), or by its number.
		std::optional<u32> keyCode(std::string_view name)
		{
			if (const auto code = number(name))
				return code;
			const std::string key = lower(name);
			if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z')
				return 4 + static_cast<u32>(key[0] - 'a');
			if (key.size() == 1 && key[0] >= '1' && key[0] <= '9')
				return 30 + static_cast<u32>(key[0] - '1');
			if (key == "0")
				return 39;
			if (key.size() >= 2 && key[0] == 'f')
				if (const auto n = number(std::string_view(key).substr(1)); n && *n >= 1 && *n <= 12)
					return 57 + *n;
			static constexpr std::pair<std::string_view, u32> Named[] = {
				{ "return", 40 }, { "enter", 40 }, { "escape", 41 }, { "esc", 41 }, { "backspace", 42 }, { "tab", 43 },
				{ "space", 44 }, { "insert", 73 }, { "home", 74 }, { "pageup", 75 }, { "delete", 76 }, { "end", 77 },
				{ "pagedown", 78 }, { "right", 79 }, { "left", 80 }, { "down", 81 }, { "up", 82 },
				{ "leftctrl", 224 }, { "leftshift", 225 }, { "rightctrl", 228 }, { "rightshift", 229 } };
			for (const auto& [text, code] : Named)
				if (key == text)
					return code;
			return std::nullopt;
		}
	}

	std::expected<std::vector<InputEvent>, std::string> parseKeyScript(std::string_view text, u64 cpuHz)
	{
		std::vector<std::pair<u64, InputEvent>> events;   // by milliseconds, with the order they were written in
		usize lineNumber = 0;
		while (!text.empty())
		{
			const usize end = text.find('\n');
			std::string_view line = text.substr(0, end);
			text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
			++lineNumber;
			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);
			const usize start = line.find_first_not_of(" \t");
			if (start == std::string_view::npos || line[start] == '#')
				continue;
			line.remove_prefix(start);

			const auto fail = [&](const std::string& why) { return std::unexpected("line " + std::to_string(lineNumber) + ": " + why); };
			const usize firstSpace = line.find_first_of(" \t");
			const auto ms = number(line.substr(0, firstSpace));
			if (!ms || firstSpace == std::string_view::npos)
				return fail("expected '<ms> <down|up|press|text> <argument>'");
			std::string_view rest = line.substr(firstSpace);
			rest.remove_prefix(std::min(rest.size(), rest.find_first_not_of(" \t")));
			const usize secondSpace = rest.find_first_of(" \t");
			const std::string action = lower(rest.substr(0, secondSpace));
			std::string_view argument = secondSpace == std::string_view::npos ? std::string_view{} : rest.substr(secondSpace + 1);

			const u64 at = u64{ *ms };
			if (action == "text")
			{
				if (argument.empty())
					return fail("'text' needs the text to type");
				events.emplace_back(at, InputEvent{ .kind = InputEvent::Kind::Text, .data = std::string(argument) });
				continue;
			}
			while (!argument.empty() && (argument.back() == ' ' || argument.back() == '\t'))
				argument.remove_suffix(1);
			const auto code = keyCode(argument);
			if (!code)
				return fail("no such key: '" + std::string(argument) + "'");
			const auto key = [&](bool pressed)
			{
				InputEvent event{ .kind = InputEvent::Kind::Key };
				event.values[0] = static_cast<i32>(*code);
				event.values[1] = pressed ? 1 : 0;
				return event;
			};
			if (action == "down")
				events.emplace_back(at, key(true));
			else if (action == "up")
				events.emplace_back(at, key(false));
			else if (action == "press")
			{
				events.emplace_back(at, key(true));
				events.emplace_back(at + 1, key(false));
			}
			else
				return fail("expected down, up, press or text, not '" + action + "'");
		}

		std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		std::vector<InputEvent> out;
		out.reserve(events.size());
		for (auto& [ms, event] : events)
		{
			event.cycle = ms / 1000 * cpuHz + (ms % 1000) * cpuHz / 1000;
			out.push_back(std::move(event));
		}
		return out;
	}
}

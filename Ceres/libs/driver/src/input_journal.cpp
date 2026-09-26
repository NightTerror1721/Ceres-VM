#include <ceres/driver/input_journal.h>
#include <charconv>
#include <istream>
#include <ostream>

namespace ceres::driver
{
	namespace
	{
		struct KindName
		{
			InputEvent::Kind kind;
			std::string_view name;
			usize values;       // how many numbers follow
			bool data;          // whether a hex string follows them
		};

		constexpr KindName Kinds[] = {
			{ InputEvent::Kind::TerminalBytes, "term", 0, true },
			{ InputEvent::Kind::TerminalClose, "term-close", 0, false },
			{ InputEvent::Kind::Key, "key", 2, false },
			{ InputEvent::Kind::Text, "text", 0, true },
			{ InputEvent::Kind::Mouse, "mouse", 4, false },
			{ InputEvent::Kind::Gamepad, "pad", 7, false },
			{ InputEvent::Kind::FileDrop, "drop", 0, true },
			{ InputEvent::Kind::Quit, "quit", 0, false },
			{ InputEvent::Kind::Reset, "reset", 0, false },
		};

		const KindName& describe(InputEvent::Kind kind)
		{
			for (const KindName& entry : Kinds)
				if (entry.kind == kind)
					return entry;
			return Kinds[0];
		}

		std::string toHex(std::string_view bytes)
		{
			constexpr char Digits[] = "0123456789abcdef";
			std::string out;
			out.reserve(bytes.size() * 2);
			for (const char c : bytes)
			{
				const auto byte = static_cast<unsigned char>(c);
				out += Digits[byte >> 4];
				out += Digits[byte & 0xF];
			}
			return out;
		}

		std::optional<std::string> fromHex(std::string_view text)
		{
			if (text.size() % 2 != 0)
				return std::nullopt;
			std::string out;
			out.reserve(text.size() / 2);
			for (usize i = 0; i < text.size(); i += 2)
			{
				unsigned value = 0;
				const auto [end, error] = std::from_chars(text.data() + i, text.data() + i + 2, value, 16);
				if (error != std::errc{} || end != text.data() + i + 2)
					return std::nullopt;
				out += static_cast<char>(value);
			}
			return out;
		}

		// The next space-separated word of `rest`, which moves past it.
		std::string_view nextWord(std::string_view& rest)
		{
			while (!rest.empty() && rest.front() == ' ')
				rest.remove_prefix(1);
			const usize end = rest.find(' ');
			const std::string_view word = rest.substr(0, end);
			rest.remove_prefix(end == std::string_view::npos ? rest.size() : end);
			return word;
		}
	}

	std::string formatInputEvent(const InputEvent& event)
	{
		const KindName& kind = describe(event.kind);
		std::string line = std::to_string(event.cycle) + ' ' + std::string(kind.name);
		for (usize i = 0; i < kind.values; ++i)
			line += ' ' + std::to_string(event.values[i]);
		if (kind.data)
			line += ' ' + toHex(event.data);
		return line;
	}

	std::optional<InputEvent> parseInputEvent(std::string_view line)
	{
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);
		InputEvent event;
		const std::string_view cycle = nextWord(line);
		if (const auto [end, error] = std::from_chars(cycle.data(), cycle.data() + cycle.size(), event.cycle);
			cycle.empty() || error != std::errc{} || end != cycle.data() + cycle.size())
			return std::nullopt;
		const std::string_view name = nextWord(line);
		const KindName* kind = nullptr;
		for (const KindName& entry : Kinds)
			if (entry.name == name)
				kind = &entry;
		if (kind == nullptr)
			return std::nullopt;
		event.kind = kind->kind;
		for (usize i = 0; i < kind->values; ++i)
		{
			const std::string_view word = nextWord(line);
			const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), event.values[i]);
			if (word.empty() || error != std::errc{} || end != word.data() + word.size())
				return std::nullopt;
		}
		if (kind->data)
		{
			auto data = fromHex(nextWord(line));
			if (!data)
				return std::nullopt;
			event.data = std::move(*data);
		}
		if (!nextWord(line).empty())
			return std::nullopt;
		return event;
	}

	std::expected<std::vector<InputEvent>, std::string> readInputRecording(std::istream& in)
	{
		std::string line;
		if (!std::getline(in, line) || (line != InputHub::RecordingHeader && line != std::string(InputHub::RecordingHeader) + '\r'))
			return std::unexpected(std::string("not a Ceres input recording (it does not start with '") + std::string(InputHub::RecordingHeader) + "')");
		std::vector<InputEvent> events;
		usize number = 1;
		while (std::getline(in, line))
		{
			++number;
			if (line.empty() || line == "\r")
				continue;
			auto event = parseInputEvent(line);
			if (!event)
				return std::unexpected("line " + std::to_string(number) + " of the recording is not an input event");
			events.push_back(std::move(*event));
		}
		return events;
	}

	void InputHub::record(std::ostream& out)
	{
		_record = &out;
		out << RecordingHeader << '\n';
	}

	void InputHub::replay(std::vector<InputEvent> events)
	{
		_replay = std::move(events);
		_nextReplay = 0;
		_replaying = true;
	}

	void InputHub::post(InputEvent event)
	{
		if (_replaying)
			return;
		const std::lock_guard lock{ _mutex };
		if (event.kind == InputEvent::Kind::TerminalBytes && !_queue.empty() && _queue.back().kind == InputEvent::Kind::TerminalBytes)
			_queue.back().data += event.data;
		else
			_queue.push_back(std::move(event));
		if (_wake != nullptr)
			_wake->poke();
	}

	void InputHub::disconnect()
	{
		const std::lock_guard lock{ _mutex };
		_wake = nullptr;
	}

	void InputHub::key(u32 code, bool pressed)
	{
		InputEvent event{ .kind = InputEvent::Kind::Key };
		event.values = { static_cast<i32>(code), pressed ? 1 : 0 };
		post(std::move(event));
	}

	void InputHub::text(std::string_view utf8)
	{
		post(InputEvent{ .kind = InputEvent::Kind::Text, .data = std::string(utf8) });
	}

	void InputHub::mouse(i32 dx, i32 dy, u8 buttons, i8 wheel)
	{
		InputEvent event{ .kind = InputEvent::Kind::Mouse };
		event.values = { dx, dy, buttons, wheel };
		post(std::move(event));
	}

	void InputHub::gamepad(u16 buttons, i16 leftX, i16 leftY, i16 rightX, i16 rightY, u16 leftTrigger, u16 rightTrigger)
	{
		InputEvent event{ .kind = InputEvent::Kind::Gamepad };
		event.values = { buttons, leftX, leftY, rightX, rightY, leftTrigger, rightTrigger };
		post(std::move(event));
	}

	void InputHub::write(const InputEvent& event)
	{
		if (_record != nullptr)
			*_record << formatInputEvent(event) << '\n';
	}

	void InputHub::apply(const InputEvent& event, InputTargets& targets)
	{
		const auto& v = event.values;
		switch (event.kind)
		{
		case InputEvent::Kind::TerminalBytes:
			targets.terminal.pushInput(std::string_view(event.data));
			break;
		case InputEvent::Kind::TerminalClose:
			targets.terminal.closeInput();
			break;
		case InputEvent::Kind::Key:
			targets.keyboard.pushKey(static_cast<u32>(v[0]), v[1] != 0);
			break;
		case InputEvent::Kind::Text:
			targets.keyboard.pushText(std::string_view(event.data));
			break;
		case InputEvent::Kind::Mouse:
			targets.mouse.pushMotion(v[0], v[1], static_cast<u8>(v[2]), static_cast<i8>(v[3]));
			break;
		case InputEvent::Kind::Gamepad:
			targets.gamepad.pushState(static_cast<u16>(v[0]), static_cast<i16>(v[1]), static_cast<i16>(v[2]), static_cast<i16>(v[3]),
				static_cast<i16>(v[4]), static_cast<u16>(v[5]), static_cast<u16>(v[6]));
			break;
		case InputEvent::Kind::FileDrop:
			if (targets.drop)   // the path is kept as UTF-8, whatever the host's own encoding
				targets.drop(std::filesystem::path(std::u8string(event.data.begin(), event.data.end())));
			break;
		case InputEvent::Kind::Quit:
		case InputEvent::Kind::Reset:
			break;
		}
	}

	bool InputHub::inject(u64 cycle, InputTargets& targets)
	{
		if (_replaying)
		{
			// Everything recorded up to this cycle, and not past a restart the machine has not made yet.
			while (_nextReplay < _replay.size())
			{
				const InputEvent& event = _replay[_nextReplay];
				if (event.kind == InputEvent::Kind::Reset || event.cycle > cycle)
					break;
				if (event.kind == InputEvent::Kind::Quit)
					return false;
				apply(event, targets);
				++_nextReplay;
			}
			return true;
		}

		std::unique_lock lock{ _mutex };
		while (!_queue.empty())
		{
			InputEvent event = std::move(_queue.front());
			_queue.pop_front();
			if (event.kind == InputEvent::Kind::TerminalBytes)
			{
				// The ring keeps one slot free; what does not fit waits, in order, for the next injection point.
				const usize available = targets.terminal.availableBytes();
				const usize room = available + 1 < devices::TerminalDevice::InputBufferCapacity
					? devices::TerminalDevice::InputBufferCapacity - 1 - available : 0;
				if (room == 0)
				{
					_queue.push_front(std::move(event));
					break;
				}
				if (event.data.size() > room)
				{
					_queue.push_front(InputEvent{ .kind = InputEvent::Kind::TerminalBytes, .data = event.data.substr(room) });
					event.data.resize(room);
				}
			}
			event.cycle = cycle;
			lock.unlock();
			apply(event, targets);
			write(event);
			lock.lock();
			// A partial write of the terminal leaves the rest at the front: stop here, the ring is full.
			if (event.kind == InputEvent::Kind::TerminalBytes && !_queue.empty() && _queue.front().kind == InputEvent::Kind::TerminalBytes
				&& targets.terminal.availableBytes() + 1 >= devices::TerminalDevice::InputBufferCapacity)
				break;
		}
		return true;
	}

	void InputHub::quit(u64 cycle)
	{
		if (!_replaying)
			write(InputEvent{ .cycle = cycle, .kind = InputEvent::Kind::Quit });
	}

	void InputHub::restarted()
	{
		if (_replaying)
		{
			if (_nextReplay < _replay.size() && _replay[_nextReplay].kind == InputEvent::Kind::Reset)
				++_nextReplay;
			return;
		}
		write(InputEvent{ .kind = InputEvent::Kind::Reset });
	}
}

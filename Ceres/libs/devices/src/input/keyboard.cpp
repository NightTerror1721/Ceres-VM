#include <ceres/devices/input/keyboard.h>

namespace ceres::devices
{
	void KeyboardDevice::pushKey(u32 code, bool pressed)
	{
		if (pressed && scancode::isNamedKey(code & EventCodeMask))
			pushKeystroke(KeyNamed | (code & EventCodeMask));
		const u32 event = (code & EventCodeMask) | (pressed ? EventPressed : 0u);
		{
			const std::lock_guard lock{_mutex};
			const usize nextTail = (_tail.load(std::memory_order_relaxed) + 1) % EventBufferCapacity;
			if (nextTail == _head.load(std::memory_order_acquire))
			{
				_droppedEvents.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			_events[_tail.load(std::memory_order_relaxed)] = event;
			_tail.store(nextTail, std::memory_order_release);
		}
		raiseInterrupt(Interrupt);
	}

	void KeyboardDevice::pushText(u32 codePoint)
	{
		if (codePoint >= 32 && codePoint != 127)
			pushKeystroke(codePoint);
		{
			const std::lock_guard lock{_mutex};
			const usize nextTail = (_textTail + 1) % EventBufferCapacity;
			if (nextTail == _textHead)
			{
				_droppedEvents.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			_text[_textTail] = codePoint;
			_textTail = nextTail;
		}
		raiseInterrupt(Interrupt);
	}

	void KeyboardDevice::pushText(std::string_view utf8)
	{
		for (usize i = 0; i < utf8.size();)
		{
			const u8 lead = static_cast<u8>(utf8[i]);
			u32 length = 1;
			u32 codePoint = lead;
			if (lead >= 0xF0 && lead < 0xF8) { length = 4; codePoint = lead & 0x07u; }
			else if (lead >= 0xE0) { length = 3; codePoint = lead & 0x0Fu; }
			else if (lead >= 0xC0) { length = 2; codePoint = lead & 0x1Fu; }
			else if (lead >= 0x80) { ++i; continue; }

			if (length > 1)
			{
				bool valid = i + length <= utf8.size();
				for (u32 k = 1; valid && k < length; ++k)
				{
					const u8 next = static_cast<u8>(utf8[i + k]);
					valid = (next & 0xC0) == 0x80;
					codePoint = (codePoint << 6) | (next & 0x3Fu);
				}
				if (!valid) { ++i; continue; }
			}

			pushText(codePoint);
			i += length;
		}
	}

	usize KeyboardDevice::availableKeys() const noexcept
	{
		const std::lock_guard lock{_mutex};
		return (_keyTail - _keyHead + EventBufferCapacity) % EventBufferCapacity;
	}

	usize KeyboardDevice::availableText() const noexcept
	{
		const std::lock_guard lock{_mutex};
		return (_textTail - _textHead + EventBufferCapacity) % EventBufferCapacity;
	}

	usize KeyboardDevice::availableEvents() const noexcept
	{
		const usize head = _head.load(std::memory_order_acquire);
		const usize tail = _tail.load(std::memory_order_acquire);
		return (tail - head + EventBufferCapacity) % EventBufferCapacity;
	}

	void KeyboardDevice::pushKeystroke(u32 keystroke)
	{
		{
			const std::lock_guard lock{_mutex};
			const usize nextTail = (_keyTail + 1) % EventBufferCapacity;
			if (nextTail != _keyHead)
			{
				_keys[_keyTail] = keystroke;
				_keyTail = nextTail;
			}
			else
				_droppedEvents.fetch_add(1, std::memory_order_relaxed);
		}
		if (_keystrokeSink)
			_keystrokeSink(keystroke);
		raiseInterrupt(Interrupt);
	}

	u32 KeyboardDevice::popEvent()
	{
		usize currentHead = _head.load(std::memory_order_relaxed);
		if (currentHead == _tail.load(std::memory_order_acquire))
			return 0;

		const u32 event = _events[currentHead];
		_head.store((currentHead + 1) % EventBufferCapacity, std::memory_order_release);
		return event;
	}

	void KeyboardDevice::blockRead(Address ramAddress, u32 size)
	{
		if (size == 0)
		{
			_blockReadCount = 0;
			return;
		}

		const std::lock_guard lock{_mutex};
		const u32 clampSize = memory().clampBlockSize(ramAddress, size);
		if (clampSize == 0)
		{
			_blockReadCount = 0;
			return;
		}

		auto buffer = memory().peekMutBytes(ramAddress, clampSize);
		usize count = 0;
		// One 32-bit event per four bytes, little-endian.
		while (count + 4 <= buffer.size())
		{
			usize currentHead = _head.load(std::memory_order_relaxed);
			if (currentHead == _tail.load(std::memory_order_acquire))
				break;

			const u32 event = _events[currentHead];
			_head.store((currentHead + 1) % EventBufferCapacity, std::memory_order_release);
			buffer[count] = static_cast<u8>(event & 0xFF);
			buffer[count + 1] = static_cast<u8>((event >> 8) & 0xFF);
			buffer[count + 2] = static_cast<u8>((event >> 16) & 0xFF);
			buffer[count + 3] = static_cast<u8>((event >> 24) & 0xFF);
			count += 4;
		}
		_blockReadCount = static_cast<u32>(count / 4);
	}

	u32 KeyboardDevice::readUnsignedWord(Address offset)
	{
		if (offset == StatusRegister)
			return (availableEvents() > 0 ? StatusDataReady : 0) | (availableText() > 0 ? StatusTextReady : 0) |
				(availableKeys() > 0 ? StatusKeyReady : 0);
		if (offset == EventRegister)
		{
			const std::lock_guard lock{_mutex};
			return popEvent();
		}
		if (offset == TextRegister)
		{
			const std::lock_guard lock{_mutex};
			if (_textHead == _textTail)
				return 0;
			const u32 codePoint = _text[_textHead];
			_textHead = (_textHead + 1) % EventBufferCapacity;
			return codePoint;
		}
		if (offset == KeyRegister)
		{
			const std::lock_guard lock{_mutex};
			if (_keyHead == _keyTail)
				return 0;
			const u32 keystroke = _keys[_keyHead];
			_keyHead = (_keyHead + 1) % EventBufferCapacity;
			return keystroke;
		}
		if (offset == BlockReadCountRegister)
			return _blockReadCount;
		return 0;
	}

	void KeyboardDevice::writeWord(Address offset, u32 value)
	{
		if (offset == BlockAddressRegister) { _blockAddress = value; return; }
		if (offset == BlockLengthRegister) { _blockLength = value; return; }
		if (offset == BlockCommandRegister && value == BlockCommandRead)
			blockRead(Address(_blockAddress), _blockLength);
	}

	std::string keystrokeToTerminalBytes(u32 keystroke)
	{
	if ((keystroke & KeyboardDevice::KeyNamed) == 0)
	{
		std::string out;
		if (keystroke < 0x80) out += static_cast<char>(keystroke);
		else if (keystroke < 0x800) { out += static_cast<char>(0xC0 | (keystroke >> 6)); out += static_cast<char>(0x80 | (keystroke & 0x3F)); }
		else if (keystroke < 0x10000)
		{
			out += static_cast<char>(0xE0 | (keystroke >> 12));
			out += static_cast<char>(0x80 | ((keystroke >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (keystroke & 0x3F));
		}
		else
		{
			out += static_cast<char>(0xF0 | (keystroke >> 18));
			out += static_cast<char>(0x80 | ((keystroke >> 12) & 0x3F));
			out += static_cast<char>(0x80 | ((keystroke >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (keystroke & 0x3F));
		}
		return out;
	}
	switch (keystroke & ~KeyboardDevice::KeyNamed)
	{
		case scancode::Return: case scancode::KeypadEnter: return "\n";
		case scancode::Escape: return "\x1b";
		case scancode::Backspace: return "\b";
		case scancode::Tab: return "\t";
		case scancode::Up: return "\x1b[A";
		case scancode::Down: return "\x1b[B";
		case scancode::Right: return "\x1b[C";
		case scancode::Left: return "\x1b[D";
		case scancode::Home: return "\x1b[H";
		case scancode::End: return "\x1b[F";
		case scancode::Insert: return "\x1b[2~";
		case scancode::Delete: return "\x1b[3~";
		case scancode::PageUp: return "\x1b[5~";
		case scancode::PageDown: return "\x1b[6~";
		default: return {};
	}
	}
}

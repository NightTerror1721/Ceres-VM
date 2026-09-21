#pragma once

// The two human-input devices the machine was missing: a keyboard that reports *events* (a code
// plus a pressed/released flag) rather than a stream of characters, and a mouse that reports
// deltas and absolute position together. Both are driven from the host the way the terminal is -
// `pushKey`/`pushMotion` - so a program sees them through the same MMIO loads and interrupts it
// already uses for everything else.

#include <ceres/vm/mmio_bus.h>
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace ceres::devices
{
	using namespace vm;

	// The key codes the keyboard reports are SDL's scancodes. These are the ones with no character of
	// their own - the keys a program has to be told about by name.
	namespace scancode
	{
		inline constexpr u32 Return = 40, Escape = 41, Backspace = 42, Tab = 43, Space = 44;
		inline constexpr u32 F1 = 58, F12 = 69;
		inline constexpr u32 Insert = 73, Home = 74, PageUp = 75, Delete = 76, End = 77, PageDown = 78;
		inline constexpr u32 Right = 79, Left = 80, Down = 81, Up = 82;
		inline constexpr u32 KeypadEnter = 88;

		// True for a key that types no character: it reaches a program as a named keystroke.
		constexpr bool isNamedKey(u32 code) noexcept
		{
			return (code >= Return && code <= Tab) || (code >= F1 && code <= F12) ||
				(code >= Insert && code <= Up) || code == KeypadEnter;
		}
	}

	// A keyboard, distinct from the terminal: the terminal delivers a stream of characters with no
	// notion of which key produced them, while this device reports events - a code plus a
	// pressed/released flag - so a game can tell a held key from a freshly pressed one, or stop an
	// action on a release instead of waiting for a repeat.
	class KeyboardDevice final : public IODevice
	{
	public:
		static inline constexpr Address StatusRegister = Address(0x00); // Read-only: bit 0 = an event is available.
		static inline constexpr Address EventRegister = Address(0x04); // Read-only: pops one event; bits 30:0 = code, bit 31 = 1 if pressed, 0 if released.
		static inline constexpr Address TextRegister = Address(0x08); // Read-only: pops one typed character as a Unicode code point; 0 when empty.
		static inline constexpr Address KeyRegister = Address(0x0C); // Read-only: pops the next keystroke, in the order it was typed; 0 when empty. A character is its code point; a key with no character (Enter, Esc, the arrows...) is KeyNamed | its scancode.
		static inline constexpr Address BlockReadCountRegister = Address(0x10); // Read-only: events drained by the last block read.

		// The same block trio the terminal and the disk use: drain the event queue into RAM as a
		// run of four-byte entries (one 32-bit event each, little-endian).
		static inline constexpr Address BlockAddressRegister = Address(0xF0);
		static inline constexpr Address BlockLengthRegister = Address(0xF4);
		static inline constexpr Address BlockCommandRegister = Address(0xF8);

		static inline constexpr u32 BlockCommandRead = 1;

		static inline constexpr u32 StatusDataReady = 1u << 0;
		static inline constexpr u32 StatusTextReady = 1u << 1;    // A typed character is waiting in the text queue.
		static inline constexpr u32 StatusKeyReady = 1u << 2;     // A keystroke is waiting in the ordered keystroke queue.
		static inline constexpr u32 KeyNamed = 1u << 31;          // In a keystroke: the low bits are the scancode of a key with no character.
		static inline constexpr u32 EventPressed = 1u << 31;      // Set when the key was pressed, clear when released.
		static inline constexpr u32 EventCodeMask = 0x7FFFFFFFu;  // The key code lives in the low 31 bits.

		// Fourth user interrupt: UserInterrupt0 is the timer's, UserInterrupt1 the terminal's,
		// UserInterrupt2 the DMA controller's.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt3;

		static inline constexpr usize EventBufferCapacity = 64;

	private:
		std::array<u32, EventBufferCapacity> _events{};
		std::atomic<usize> _head{0};
		std::atomic<usize> _tail{0};
		std::atomic<u64> _droppedEvents{0};
		// Typed characters, kept apart from the key events: the events say which physical key moved,
		// the text says what the layout made of it - capitals, accents, dead keys, IME input - which
		// a program cannot rebuild from the scan codes.
		std::array<u32, EventBufferCapacity> _text{};
		usize _textHead = 0;
		usize _textTail = 0;
		// Keystrokes: what a person typed, in the order they typed it. The two queues above cannot say
		// whether the letter or the Enter came first, because they are separate; this one merges them - a
		// typed character, or a key that has no character - so a text field or a menu reads one stream.
		std::array<u32, EventBufferCapacity> _keys{};
		usize _keyHead = 0;
		usize _keyTail = 0;
		std::function<void(u32)> _keystrokeSink;
		mutable std::mutex _mutex;
		u32 _blockAddress = 0;
		u32 _blockLength = 0;
		u32 _blockReadCount = 0;

	public:
		KeyboardDevice() = default;
		KeyboardDevice(const KeyboardDevice&) = delete;
		KeyboardDevice(KeyboardDevice&&) = delete;
		~KeyboardDevice() override = default;

		KeyboardDevice& operator=(const KeyboardDevice&) = delete;
		KeyboardDevice& operator=(KeyboardDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Keyboard, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Keyboard);
		}

		// A host keyboard feeds the device one event at a time. `code` is whatever the host maps a
		// physical key to - a scan code, or the ASCII value of a character - and `pressed` false
		// marks a release. The low 31 bits of `code` are kept, so SDL scancodes fit comfortably.
		void pushKey(u32 code, bool pressed = true)
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

		// A typed character, as the Unicode code point the host's layout produced. It raises the
		// keyboard's interrupt like a key event does, and a full queue drops it.
		void pushText(u32 codePoint)
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

		// The same, for a host that hands over a UTF-8 string (SDL's text input event does). A
		// malformed sequence is skipped a byte at a time rather than guessed at.
		void pushText(std::string_view utf8)
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

		// Called with each keystroke as it is queued, from the host's thread. A windowed host uses it to hand
		// the keystrokes to the terminal as bytes as well, for a program that reads its input that way.
		void setKeystrokeSink(std::function<void(u32)> sink) { _keystrokeSink = std::move(sink); }

		u64 droppedEvents() const noexcept { return _droppedEvents.load(std::memory_order_relaxed); }

		usize availableKeys() const noexcept
		{
			const std::lock_guard lock{_mutex};
			return (_keyTail - _keyHead + EventBufferCapacity) % EventBufferCapacity;
		}

		usize availableText() const noexcept
		{
			const std::lock_guard lock{_mutex};
			return (_textTail - _textHead + EventBufferCapacity) % EventBufferCapacity;
		}

		usize availableEvents() const noexcept
		{
			const usize head = _head.load(std::memory_order_acquire);
			const usize tail = _tail.load(std::memory_order_acquire);
			return (tail - head + EventBufferCapacity) % EventBufferCapacity;
		}

	private:
		void pushKeystroke(u32 keystroke)
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

		// Pops one event from the ring; returns 0 when empty. The caller holds the mutex.
		u32 popEvent()
		{
			usize currentHead = _head.load(std::memory_order_relaxed);
			if (currentHead == _tail.load(std::memory_order_acquire))
				return 0;

			const u32 event = _events[currentHead];
			_head.store((currentHead + 1) % EventBufferCapacity, std::memory_order_release);
			return event;
		}

		void blockRead(Address ramAddress, u32 size)
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

	public:
		u32 readUnsignedWord(Address offset) override
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
		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		void writeWord(Address offset, u32 value) override
		{
			if (offset == BlockAddressRegister) { _blockAddress = value; return; }
			if (offset == BlockLengthRegister) { _blockLength = value; return; }
			if (offset == BlockCommandRegister && value == BlockCommandRead)
				blockRead(Address(_blockAddress), _blockLength);
		}
		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }
	};

	// What a keystroke is as bytes on a terminal: a character as UTF-8, and each named key as the byte or the
	// escape sequence a terminal sends for it. A windowed host feeds these to the terminal so a program that
	// reads its input as a stream still sees what was typed.
	inline std::string keystrokeToTerminalBytes(u32 keystroke)
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

	// A mouse, reporting movement two ways at once: a delta since the program last read it (the
	// thing a game wants frame to frame) and an absolute position accumulated from every motion
	// (the thing an editor wants). The button mask and wheel are latched alongside.
	class MouseDevice final : public IODevice
	{
	public:
		static inline constexpr Address StatusRegister = Address(0x00); // Read-only: bit 0 = new motion/button data since the last status read.
		static inline constexpr Address DeltaXRegister = Address(0x04); // Read-only: signed movement in X since the last read (consumed on read).
		static inline constexpr Address DeltaYRegister = Address(0x08); // Read-only: signed movement in Y since the last read (consumed on read).
		static inline constexpr Address XRegister = Address(0x0C); // Read-only: absolute X.
		static inline constexpr Address YRegister = Address(0x10); // Read-only: absolute Y.
		static inline constexpr Address ButtonsRegister = Address(0x14); // Read-only: button mask.
		static inline constexpr Address WheelRegister = Address(0x18); // Read-only: wheel movement since the last read (consumed on read).

		static inline constexpr u32 StatusDataReady = 1u << 0;
		static inline constexpr u8 ButtonLeft = 1u << 0;
		static inline constexpr u8 ButtonRight = 1u << 1;
		static inline constexpr u8 ButtonMiddle = 1u << 2;

		// Fifth user interrupt: the timer, terminal, DMA controller and keyboard take 0-3.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt4;

	private:
		i32 _x = 0;
		i32 _y = 0;
		i32 _dx = 0;
		i32 _dy = 0;
		i32 _wheelDelta = 0;
		u8 _buttons = 0;
		bool _updated = false;
		mutable std::mutex _mutex;

	public:
		MouseDevice() = default;
		MouseDevice(const MouseDevice&) = delete;
		MouseDevice(MouseDevice&&) = delete;
		~MouseDevice() override = default;

		MouseDevice& operator=(const MouseDevice&) = delete;
		MouseDevice& operator=(MouseDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Mouse, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Mouse);
		}

		// The host reports movement since the last time it sampled the physical pointer. Deltas
		// accumulate into both the latched delta (for the next delta read) and the absolute
		// position; the button mask and wheel are latched the same way.
		void pushMotion(i32 dx, i32 dy, u8 buttons = 0, i8 wheel = 0)
		{
			{
				const std::lock_guard lock{_mutex};
				_dx += dx;
				_dy += dy;
				_x += dx;
				_y += dy;
				_wheelDelta += wheel;
				_buttons = buttons;
				_updated = true;
			}
			raiseInterrupt(Interrupt);
		}

	public:
		u32 readUnsignedWord(Address offset) override
		{
			if (offset == StatusRegister)
			{
				const std::lock_guard lock{_mutex};
				const u32 status = _updated ? StatusDataReady : 0;
				_updated = false;
				return status;
			}
			if (offset == DeltaXRegister) { const std::lock_guard lock{_mutex}; const i32 v = _dx; _dx = 0; return static_cast<u32>(v); }
			if (offset == DeltaYRegister) { const std::lock_guard lock{_mutex}; const i32 v = _dy; _dy = 0; return static_cast<u32>(v); }
			if (offset == XRegister) { const std::lock_guard lock{_mutex}; return static_cast<u32>(_x); }
			if (offset == YRegister) { const std::lock_guard lock{_mutex}; return static_cast<u32>(_y); }
			if (offset == ButtonsRegister) { const std::lock_guard lock{_mutex}; return _buttons; }
			if (offset == WheelRegister) { const std::lock_guard lock{_mutex}; const i32 v = _wheelDelta; _wheelDelta = 0; return static_cast<u32>(v); }
			return 0;
		}
		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		void writeByte(Address, u8) override {}
		void writeHalfword(Address, u16) override {}
		void writeWord(Address, u32) override {}
	};

	// A gamepad, polled rather than event-driven: a game loop reads the button mask and the axes
	// every frame instead of draining a queue. The host pushes the current state with pushState();
	// a state that actually changed raises UserInterrupt5, so a program can also wait on it.
	class GamepadDevice final : public IODevice
	{
	public:
		static inline constexpr Address StatusRegister = Address(0x00);        // Read: bit 0 = state changed since the last status read (consumed on read).
		static inline constexpr Address ButtonsRegister = Address(0x04);       // Read: button bitmask.
		static inline constexpr Address LeftXRegister = Address(0x08);         // Read: signed left stick X (-32768..32767).
		static inline constexpr Address LeftYRegister = Address(0x0C);         // Read: signed left stick Y.
		static inline constexpr Address RightXRegister = Address(0x10);        // Read: signed right stick X.
		static inline constexpr Address RightYRegister = Address(0x14);        // Read: signed right stick Y.
		static inline constexpr Address LeftTriggerRegister = Address(0x18);   // Read: left trigger (0..32767).
		static inline constexpr Address RightTriggerRegister = Address(0x1C);  // Read: right trigger (0..32767).

		static inline constexpr u32 StatusChanged = 1u << 0;

		// Buttons, laid out the way a standard gamepad reports them (south = A/cross, east =
		// B/circle, west = X/square, north = Y/triangle) so an SDL mapping is a straight lookup.
		static inline constexpr u16 ButtonSouth = 1u << 0;
		static inline constexpr u16 ButtonEast = 1u << 1;
		static inline constexpr u16 ButtonWest = 1u << 2;
		static inline constexpr u16 ButtonNorth = 1u << 3;
		static inline constexpr u16 ButtonBack = 1u << 4;
		static inline constexpr u16 ButtonGuide = 1u << 5;
		static inline constexpr u16 ButtonStart = 1u << 6;
		static inline constexpr u16 ButtonLeftStick = 1u << 7;
		static inline constexpr u16 ButtonRightStick = 1u << 8;
		static inline constexpr u16 ButtonLeftShoulder = 1u << 9;
		static inline constexpr u16 ButtonRightShoulder = 1u << 10;
		static inline constexpr u16 ButtonDpadUp = 1u << 11;
		static inline constexpr u16 ButtonDpadDown = 1u << 12;
		static inline constexpr u16 ButtonDpadLeft = 1u << 13;
		static inline constexpr u16 ButtonDpadRight = 1u << 14;

		// Sixth user interrupt: the timer, terminal, DMA controller, keyboard and mouse take 0-4.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt5;

	private:
		u16 _buttons = 0;
		i16 _leftX = 0;
		i16 _leftY = 0;
		i16 _rightX = 0;
		i16 _rightY = 0;
		u16 _leftTrigger = 0;
		u16 _rightTrigger = 0;
		bool _changed = false;
		mutable std::mutex _mutex;

	public:
		GamepadDevice() = default;
		GamepadDevice(const GamepadDevice&) = delete;
		GamepadDevice(GamepadDevice&&) = delete;
		~GamepadDevice() override = default;

		GamepadDevice& operator=(const GamepadDevice&) = delete;
		GamepadDevice& operator=(GamepadDevice&&) = delete;

	public:
		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Gamepad, *this);
		}

		void detachFrom(MmioBus& bus)
		{
			bus.detach(default_mmio::Gamepad);
		}

		// The host reports the whole current state. A state that differs from the last one marks the
		// device changed and raises the interrupt; a held button or a resting stick raises nothing.
		void pushState(u16 buttons, i16 leftX, i16 leftY, i16 rightX, i16 rightY, u16 leftTrigger, u16 rightTrigger)
		{
			bool changed = false;
			{
				const std::lock_guard lock{_mutex};
				changed = buttons != _buttons || leftX != _leftX || leftY != _leftY ||
					rightX != _rightX || rightY != _rightY ||
					leftTrigger != _leftTrigger || rightTrigger != _rightTrigger;

				_buttons = buttons;
				_leftX = leftX;
				_leftY = leftY;
				_rightX = rightX;
				_rightY = rightY;
				_leftTrigger = leftTrigger;
				_rightTrigger = rightTrigger;
				if (changed)
					_changed = true;
			}
			if (changed)
				raiseInterrupt(Interrupt);
		}

	public:
		u32 readUnsignedWord(Address offset) override
		{
			const std::lock_guard lock{_mutex};

			if (offset == StatusRegister)
			{
				const u32 status = _changed ? StatusChanged : 0;
				_changed = false;
				return status;
			}
			if (offset == ButtonsRegister) return _buttons;
			if (offset == LeftXRegister) return static_cast<u32>(static_cast<i32>(_leftX));
			if (offset == LeftYRegister) return static_cast<u32>(static_cast<i32>(_leftY));
			if (offset == RightXRegister) return static_cast<u32>(static_cast<i32>(_rightX));
			if (offset == RightYRegister) return static_cast<u32>(static_cast<i32>(_rightY));
			if (offset == LeftTriggerRegister) return _leftTrigger;
			if (offset == RightTriggerRegister) return _rightTrigger;
			return 0;
		}
		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		void writeByte(Address, u8) override {}
		void writeHalfword(Address, u16) override {}
		void writeWord(Address, u32) override {}
	};
}

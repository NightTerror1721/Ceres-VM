#pragma once

// The keyboard: key events for a program that wants keys rather than characters, the scan codes it reports,
// and what a keystroke is as bytes on a terminal.

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
		void pushKey(u32 code, bool pressed = true);

		// A typed character, as the Unicode code point the host's layout produced. It raises the
		// keyboard's interrupt like a key event does, and a full queue drops it.
		void pushText(u32 codePoint);

		// The same, for a host that hands over a UTF-8 string (SDL's text input event does). A
		// malformed sequence is skipped a byte at a time rather than guessed at.
		void pushText(std::string_view utf8);

		// Called with each keystroke as it is queued, from the host's thread. A windowed host uses it to hand
		// the keystrokes to the terminal as bytes as well, for a program that reads its input that way.
		void setKeystrokeSink(std::function<void(u32)> sink) { _keystrokeSink = std::move(sink); }

		u64 droppedEvents() const noexcept { return _droppedEvents.load(std::memory_order_relaxed); }

		usize availableKeys() const noexcept;

		usize availableText() const noexcept;

		usize availableEvents() const noexcept;

	private:
		void pushKeystroke(u32 keystroke);

		// Pops one event from the ring; returns 0 when empty. The caller holds the mutex.
		u32 popEvent();

		void blockRead(Address ramAddress, u32 size);

	public:
		u32 readUnsignedWord(Address offset) override;
		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		void writeWord(Address offset, u32 value) override;
		void writeByte(Address offset, u8 value) override { writeWord(offset, value); }
		void writeHalfword(Address offset, u16 value) override { writeWord(offset, value); }
	};

	// What a keystroke is as bytes on a terminal: a character as UTF-8, and each named key as the byte or the
	// escape sequence a terminal sends for it. A windowed host feeds these to the terminal so a program that
	// reads its input as a stream still sees what was typed.
	std::string keystrokeToTerminalBytes(u32 keystroke);
}

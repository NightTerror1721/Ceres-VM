#pragma once

// The two human-input devices the machine was missing: a keyboard that reports *events* (a code
// plus a pressed/released flag) rather than a stream of characters, and a mouse that reports
// deltas and absolute position together. Both are driven from the host the way the terminal is -
// `pushKey`/`pushMotion` - so a program sees them through the same MMIO loads and interrupts it
// already uses for everything else.

#include <ceres/vm/mmio_bus.h>
#include <array>
#include <atomic>
#include <mutex>
#include <span>

namespace ceres::devices
{
	using namespace vm;

	// A keyboard, distinct from the terminal: the terminal delivers a stream of characters with no
	// notion of which key produced them, while this device reports events - a code plus a
	// pressed/released flag - so a game can tell a held key from a freshly pressed one, or stop an
	// action on a release instead of waiting for a repeat.
	class KeyboardDevice final : public IODevice
	{
	public:
		static inline constexpr Address StatusRegister = Address(0x00); // Read-only: bit 0 = an event is available.
		static inline constexpr Address EventRegister = Address(0x04); // Read-only: pops one event; low byte = code, bit 8 = 1 if pressed, 0 if released.
		static inline constexpr Address BlockReadCountRegister = Address(0x10); // Read-only: events drained by the last block read.

		// The same block trio the terminal and the disk use: drain the event queue into RAM as a
		// run of two-byte entries (code, then pressed flag).
		static inline constexpr Address BlockAddressRegister = Address(0xF0);
		static inline constexpr Address BlockLengthRegister = Address(0xF4);
		static inline constexpr Address BlockCommandRegister = Address(0xF8);

		static inline constexpr u32 BlockCommandRead = 1;

		static inline constexpr u32 StatusDataReady = 1u << 0;
		static inline constexpr u16 EventPressed = 1u << 8;

		// Fourth user interrupt: UserInterrupt0 is the timer's, UserInterrupt1 the terminal's,
		// UserInterrupt2 the DMA controller's.
		static inline constexpr InterruptNumber Interrupt = InterruptNumber::UserInterrupt3;

		static inline constexpr usize EventBufferCapacity = 64;

	private:
		std::array<u16, EventBufferCapacity> _events{};
		std::atomic<usize> _head{0};
		std::atomic<usize> _tail{0};
		std::atomic<u64> _droppedEvents{0};
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
		// marks a release.
		void pushKey(u8 code, bool pressed = true)
		{
			const u16 event = static_cast<u16>(code) | (pressed ? EventPressed : u16{0});
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

		u64 droppedEvents() const noexcept { return _droppedEvents.load(std::memory_order_relaxed); }

		usize availableEvents() const noexcept
		{
			const usize head = _head.load(std::memory_order_acquire);
			const usize tail = _tail.load(std::memory_order_acquire);
			return (tail - head + EventBufferCapacity) % EventBufferCapacity;
		}

	private:
		// Pops one event from the ring; returns 0 when empty. The caller holds the mutex.
		u16 popEvent()
		{
			usize currentHead = _head.load(std::memory_order_relaxed);
			if (currentHead == _tail.load(std::memory_order_acquire))
				return 0;

			const u16 event = _events[currentHead];
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
			auto buffer = memory().peekMutBytes(ramAddress, size);
			usize count = 0;
			// Two bytes per event: the code, then the pressed flag.
			while (count + 2 <= buffer.size())
			{
				usize currentHead = _head.load(std::memory_order_relaxed);
				if (currentHead == _tail.load(std::memory_order_acquire))
					break;

				const u16 event = _events[currentHead];
				_head.store((currentHead + 1) % EventBufferCapacity, std::memory_order_release);
				buffer[count] = static_cast<u8>(event & 0xFF);
				buffer[count + 1] = static_cast<u8>((event >> 8) & 0xFF);
				count += 2;
			}
			_blockReadCount = static_cast<u32>(count / 2);
		}

	public:
		u32 readUnsignedWord(Address offset) override
		{
			if (offset == StatusRegister)
				return availableEvents() > 0 ? StatusDataReady : 0;
			if (offset == EventRegister)
			{
				const std::lock_guard lock{_mutex};
				return popEvent();
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
}

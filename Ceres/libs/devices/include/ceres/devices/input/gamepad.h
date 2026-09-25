#pragma once

// The gamepad: buttons, sticks and triggers, polled.

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
		void pushState(u16 buttons, i16 leftX, i16 leftY, i16 rightX, i16 rightY, u16 leftTrigger, u16 rightTrigger);

	public:
		u32 readUnsignedWord(Address offset) override;
		u8 readUnsignedByte(Address offset) override { return static_cast<u8>(readUnsignedWord(offset)); }
		i8 readSignedByte(Address offset) override { return static_cast<i8>(readUnsignedByte(offset)); }
		u16 readUnsignedHalfword(Address offset) override { return static_cast<u16>(readUnsignedWord(offset)); }
		i16 readSignedHalfword(Address offset) override { return static_cast<i16>(readUnsignedHalfword(offset)); }

		void writeByte(Address, u8) override {}
		void writeHalfword(Address, u16) override {}
		void writeWord(Address, u32) override {}
	};
}

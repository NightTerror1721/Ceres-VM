#pragma once

// The mouse: movement as a delta and as a position, buttons and wheel.

#include <ceres/vm/mmio_bus.h>
#include <mutex>

namespace ceres::devices
{
	using namespace vm;

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
		void pushMotion(i32 dx, i32 dy, u8 buttons = 0, i8 wheel = 0);

	public:
		u32 read(Address offset) override;
		void write(Address, u32) override {}
		const RegisterMap& registers() const override;
	};
}

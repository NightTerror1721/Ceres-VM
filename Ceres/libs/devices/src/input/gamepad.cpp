#include <ceres/devices/input/gamepad.h>

namespace ceres::devices
{
	namespace
	{
		// Every register of the device (plan/v2 SPEC 5.3), in offset order.
		constexpr RegisterInfo Registers[] = {
			{ 0x00, "Status",       RegisterAccess::Read,      0x0, true,  "Bit 0 = state changed since the last status read (consumed on read)." },
			{ 0x04, "Buttons",      RegisterAccess::Read,      0x0, false, "Button bitmask." },
			{ 0x08, "LeftX",        RegisterAccess::Read,      0x0, false, "Signed left stick X (-32768..32767)." },
			{ 0x0C, "LeftY",        RegisterAccess::Read,      0x0, false, "Signed left stick Y." },
			{ 0x10, "RightX",       RegisterAccess::Read,      0x0, false, "Signed right stick X." },
			{ 0x14, "RightY",       RegisterAccess::Read,      0x0, false, "Signed right stick Y." },
			{ 0x18, "LeftTrigger",  RegisterAccess::Read,      0x0, false, "Left trigger (0..32767)." },
			{ 0x1C, "RightTrigger", RegisterAccess::Read,      0x0, false, "Right trigger (0..32767)." },
		};
	}

	void GamepadDevice::pushState(u16 buttons, i16 leftX, i16 leftY, i16 rightX, i16 rightY, u16 leftTrigger, u16 rightTrigger)
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

	u32 GamepadDevice::read(Address offset)
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

	const RegisterMap& GamepadDevice::registers() const
	{
		static constexpr RegisterMap map{ "gamepad", Registers };
		return map;
	}
}

#include <ceres/devices/input/gamepad.h>

namespace ceres::devices
{
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

	u32 GamepadDevice::readUnsignedWord(Address offset)
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
}

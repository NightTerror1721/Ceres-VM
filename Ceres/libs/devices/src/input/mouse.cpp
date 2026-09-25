#include <ceres/devices/input/mouse.h>

namespace ceres::devices
{
	void MouseDevice::pushMotion(i32 dx, i32 dy, u8 buttons, i8 wheel)
	{
		// Only a push that changes something is news: a button event with the same mask, or a wheel event
		// that moved nothing, raises nothing (docs/07), as the gamepad does.
		bool changed = false;
		{
			const std::lock_guard lock{_mutex};
			changed = dx != 0 || dy != 0 || wheel != 0 || buttons != _buttons;
			_dx += dx;
			_dy += dy;
			_x += dx;
			_y += dy;
			_wheelDelta += wheel;
			_buttons = buttons;
			if (changed)
				_updated = true;
		}
		if (changed)
			raiseInterrupt(Interrupt);
	}

	u32 MouseDevice::read(Address offset)
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
}

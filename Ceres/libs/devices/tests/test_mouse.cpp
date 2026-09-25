// The mouse (MouseDevice): the device on its own, and a program reaching it through its registers.
#include "device_test_machine.h"

// --- The mouse ---------------------------------------------------------------------------------

TEST(mouse, the_mouse_reports_deltas_and_absolute_position)
{
	MouseDevice mouse{};

	CHECK_EQ(mouse.read(MouseDevice::StatusRegister), u32{ 0 });

	mouse.pushMotion(5, -3, MouseDevice::ButtonLeft, 0);

	CHECK_EQ(mouse.read(MouseDevice::StatusRegister), MouseDevice::StatusDataReady);
	CHECK_EQ(mouse.read(MouseDevice::DeltaXRegister), u32{ 5 });
	CHECK_EQ(mouse.read(MouseDevice::DeltaYRegister), static_cast<u32>(-3));
	CHECK_EQ(mouse.read(MouseDevice::XRegister), u32{ 5 });
	CHECK_EQ(mouse.read(MouseDevice::YRegister), static_cast<u32>(-3));
	CHECK_EQ(mouse.read(MouseDevice::ButtonsRegister), u32{ MouseDevice::ButtonLeft });
}

TEST(mouse, mouse_deltas_are_consumed_on_read_but_position_is_not)
{
	MouseDevice mouse{};

	mouse.pushMotion(2, 2);

	CHECK_EQ(mouse.read(MouseDevice::DeltaXRegister), u32{ 2 });
	CHECK_EQ(mouse.read(MouseDevice::DeltaXRegister), u32{ 0 }); // Consumed.
	CHECK_EQ(mouse.read(MouseDevice::XRegister), u32{ 2 });      // Absolute persists.
}

TEST(mouse, the_mouse_accumulates_motion_across_push_calls)
{
	MouseDevice mouse{};

	mouse.pushMotion(1, 1);
	mouse.pushMotion(2, 2);

	CHECK_EQ(mouse.read(MouseDevice::DeltaXRegister), u32{ 3 }); // Accumulated delta.
	CHECK_EQ(mouse.read(MouseDevice::XRegister), u32{ 3 });      // Accumulated position.
}

TEST(mouse, the_mouse_reports_buttons_and_wheel)
{
	MouseDevice mouse{};

	mouse.pushMotion(0, 0, MouseDevice::ButtonRight | MouseDevice::ButtonMiddle, 2);

	CHECK_EQ(mouse.read(MouseDevice::ButtonsRegister), u32{ MouseDevice::ButtonRight | MouseDevice::ButtonMiddle });
	CHECK_EQ(mouse.read(MouseDevice::WheelRegister), u32{ 2 });
	CHECK_EQ(mouse.read(MouseDevice::WheelRegister), u32{ 0 }); // Consumed.
}

TEST(mouse, a_mouse_push_that_changes_nothing_is_not_news)
{
	MouseDevice mouse{};
	mouse.pushMotion(3, 0, MouseDevice::ButtonLeft, 0);
	CHECK_EQ(mouse.read(MouseDevice::StatusRegister), u32{ MouseDevice::StatusDataReady });

	// The same button mask again, no motion, no wheel: a program waiting for news would only spin on it.
	mouse.pushMotion(0, 0, MouseDevice::ButtonLeft, 0);
	CHECK_EQ(mouse.read(MouseDevice::StatusRegister), u32{ 0 });
}

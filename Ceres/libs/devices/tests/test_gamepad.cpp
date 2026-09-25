// The gamepad (GamepadDevice): the device on its own, and a program reaching it through its registers.
#include "device_test_machine.h"

// --- The gamepad -------------------------------------------------------------------------------

TEST(gamepad, the_gamepad_reports_buttons_and_axes)
{
	GamepadDevice gamepad{};

	gamepad.pushState(GamepadDevice::ButtonSouth | GamepadDevice::ButtonDpadRight, -100, 200, 0, -32768, 32767, 0);

	CHECK_EQ(gamepad.read(GamepadDevice::ButtonsRegister),
		u32{ GamepadDevice::ButtonSouth | GamepadDevice::ButtonDpadRight });
	CHECK_EQ(gamepad.read(GamepadDevice::LeftXRegister), static_cast<u32>(-100));
	CHECK_EQ(gamepad.read(GamepadDevice::LeftYRegister), u32{ 200 });
	CHECK_EQ(gamepad.read(GamepadDevice::RightYRegister), static_cast<u32>(-32768));
	CHECK_EQ(gamepad.read(GamepadDevice::LeftTriggerRegister), u32{ 32767 });
	CHECK_EQ(gamepad.read(GamepadDevice::RightTriggerRegister), u32{ 0 });
}

TEST(gamepad, a_gamepad_state_change_is_reported_until_read)
{
	GamepadDevice gamepad{};

	CHECK_EQ(gamepad.read(GamepadDevice::StatusRegister), u32{ 0 });

	// The resting state is not a change.
	gamepad.pushState(0, 0, 0, 0, 0, 0, 0);
	CHECK_EQ(gamepad.read(GamepadDevice::StatusRegister), u32{ 0 });

	gamepad.pushState(GamepadDevice::ButtonSouth, 0, 0, 0, 0, 0, 0);
	CHECK_EQ(gamepad.read(GamepadDevice::StatusRegister) & GamepadDevice::StatusChanged, GamepadDevice::StatusChanged);

	// Reading the status consumes the change.
	CHECK_EQ(gamepad.read(GamepadDevice::StatusRegister), u32{ 0 });
}

TEST(gamepad, a_gamepad_state_change_raises_the_gamepads_interrupt)
{
	Machine m{ Instruction::STI(), Instruction::NOP(), Instruction::NOP() };

	GamepadDevice gamepad{};
	gamepad.attachTo(m.vm().io());

	m.installHandler(GamepadDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x71),
		Instruction::IRET(),
	});

	gamepad.pushState(GamepadDevice::ButtonSouth, 0, 0, 0, 0, 0, 0);
	m.step(3);

	CHECK_EQ(m.reg(9), 0x71u);
}

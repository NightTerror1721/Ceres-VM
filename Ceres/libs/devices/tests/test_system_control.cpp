// The system control (SystemControlDevice): the device on its own, and a program reaching it through its registers.
#include "device_test_machine.h"

// --- An exit status ----------------------------------------------------------------------------

TEST(system_control, a_word_written_to_the_control_register_carries_the_exit_status)
{
	Machine m{
		LoadBase(default_mmio::SystemControl), LoadBaseLow(default_mmio::SystemControl),
		Instruction::LI(1, 0x0501),
		Instruction::STR(Base, 1, Off(SystemControlDevice::CommandRegister)),
	};

	bool shutDown = false;
	SystemControlDevice control{ [&] { shutDown = true; }, {} };
	control.attachTo(m.vm().io());
	m.step(4);

	CHECK(shutDown);
	CHECK_EQ(control.exitCode(), u8{ 5 });
}

TEST(system_control, a_plain_shutdown_is_status_zero)
{
	Machine m{
		LoadBase(default_mmio::SystemControl), LoadBaseLow(default_mmio::SystemControl),
		Instruction::LI(1, 1),
		Instruction::STR(Base, 1, Off(SystemControlDevice::CommandRegister)),
	};

	bool shutDown = false;
	SystemControlDevice control{ [&] { shutDown = true; }, {} };
	control.attachTo(m.vm().io());
	m.step(4);

	CHECK(shutDown);
	CHECK_EQ(control.exitCode(), u8{ 0 });
}

TEST(system_control, a_word_shutdown_carries_the_status_in_its_second_byte)
{
	SystemControlDevice control{ [] {}, {} };
	control.write(SystemControlDevice::CommandRegister, 0x2A01);
	CHECK_EQ(control.exitCode(), u8{ 42 });
}

TEST(system_control, a_reset_does_not_change_the_status)
{
	bool reset = false;
	SystemControlDevice control{ [] {}, [&] { reset = true; } };
	control.write(SystemControlDevice::CommandRegister, 0x0702);
	CHECK(reset);
	CHECK_EQ(control.exitCode(), u8{ 0 });
}

// --- What the machine has ----------------------------------------------------------------------

TEST(system_control, the_control_device_reports_how_much_ram_there_is)
{
	Machine m{
		LoadBase(default_mmio::SystemControl), LoadBaseLow(default_mmio::SystemControl),
		Instruction::LDR(1, Base, Off(SystemControlDevice::MemorySizeRegister)),
	};

	SystemControlDevice control{};
	control.attachTo(m.vm().io());
	m.step(3);

	CHECK_EQ(m.reg(1), static_cast<u32>(m.memory().size()));
}

TEST(system_control, the_control_register_stays_unreadable_and_unknown_offsets_read_all_ones)
{
	SystemControlDevice control{};
	CHECK_EQ(control.read(SystemControlDevice::CommandRegister), 0xFFFFFFFFu);
	CHECK_EQ(control.read(Address(0x40)), 0xFFFFFFFFu);
}

TEST(system_control, the_features_register_holds_what_was_written_and_tells_the_host)
{
	SystemControlDevice control{};
	u32 seen = 0;
	control.setFeaturesCallback([&](u32 features) { seen = features; });

	CHECK_EQ(control.features(), 0u);
	control.write(SystemControlDevice::FeaturesRegister, SystemControlDevice::FeatureDivisionFault);

	CHECK_EQ(seen, SystemControlDevice::FeatureDivisionFault);
	CHECK_EQ(control.read(SystemControlDevice::FeaturesRegister), SystemControlDevice::FeatureDivisionFault);
}

TEST(system_control, a_program_switches_ieee_division_on_through_the_control_device)
{
	Machine m{
		LoadBase(default_mmio::SystemControl), LoadBaseLow(default_mmio::SystemControl),
		Instruction::LI(1, static_cast<u16>(SystemControlDevice::FeatureIeeeDivide)),
		Instruction::STR(Base, 1, Off(SystemControlDevice::FeaturesRegister)),
	};

	SystemControlDevice control{};
	control.setFeaturesCallback([&](u32 features)
	{
		m.vm().engine().setDivisionFaults((features & SystemControlDevice::FeatureDivisionFault) != 0);
		m.vm().engine().setIeeeDivide((features & SystemControlDevice::FeatureIeeeDivide) != 0);
	});
	control.attachTo(m.vm().io());
	m.step(4);

	CHECK(m.vm().engine().ieeeDivide());
	CHECK(!m.vm().engine().divisionFaults());
	CHECK_EQ(SystemControlDevice::FeatureIeeeDivide, 2u);
}

TEST(system_control, a_program_switches_the_option_on_through_the_control_device)
{
	Machine m{
		LoadBase(default_mmio::SystemControl), LoadBaseLow(default_mmio::SystemControl),
		Instruction::LI(1, static_cast<u16>(SystemControlDevice::FeatureDivisionFault)),
		Instruction::STR(Base, 1, Off(SystemControlDevice::FeaturesRegister)),
	};

	SystemControlDevice control{};
	control.setFeaturesCallback([&](u32 features)
	{
		m.vm().engine().setDivisionFaults((features & SystemControlDevice::FeatureDivisionFault) != 0);
	});
	control.attachTo(m.vm().io());
	m.step(4);

	CHECK(m.vm().engine().divisionFaults());
}

TEST(system_control, a_program_moves_the_stack_limit_through_the_control_device)
{
	const u32 image = static_cast<u32>(Memory::UnrestrictedSegmentStartValue) + 0x1000;
	Machine m{
		LoadBase(default_mmio::SystemControl), LoadBaseLow(default_mmio::SystemControl),
		Instruction::LI(1, 0x2000),
		Instruction::STR(Base, 1, Off(SystemControlDevice::StackLimitRegister)),
		Instruction::LDR(2, Base, Off(SystemControlDevice::StackLimitRegister)),
	};
	m.vm().engine().setStackLimit(image);

	SystemControlDevice control{};
	CHECK_EQ(control.read(SystemControlDevice::StackLimitRegister), 0xFFFFFFFFu);   // no engine connected
	control.setStackLimitHandlers([&] { return m.vm().engine().stackLimit(); },
		[&](u32 address) { m.vm().engine().setProgramStackLimit(address); });
	control.attachTo(m.vm().io());
	m.step(5);

	CHECK_EQ(m.vm().engine().stackLimit(), 0x2000u);
	CHECK_EQ(m.reg(2), 0x2000u);
}

TEST(system_control, the_control_device_reads_the_last_fault_back)
{
	SystemControlDevice control{};
	CHECK_EQ(control.read(SystemControlDevice::FaultAddressRegister), 0xFFFFFFFFu);   // nothing connected
	control.setFaultInfoHandlers([] { return 0x801u; }, [] { return 2u | (4u << 8); }, [] { return 1u; });
	CHECK_EQ(control.read(SystemControlDevice::FaultAddressRegister), 0x801u);
	CHECK_EQ(control.read(SystemControlDevice::FaultAccessRegister), 2u | (4u << 8));
	CHECK_EQ(control.read(SystemControlDevice::FaultReasonRegister), 1u);
}

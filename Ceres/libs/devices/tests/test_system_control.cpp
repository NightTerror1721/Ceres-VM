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

// --- Load and run (command 3, plan/v2 F7.1) ------------------------------------------------------

namespace
{
	// A machine's RAM with the control device on it, and a place to write strings and words.
	struct LoadRam
	{
		CeresVM vm;
		SystemControlDevice control;
		std::optional<std::expected<SystemControlDevice::LoadRequest, std::string>> seen;

		LoadRam()
		{
			control.attachTo(vm.io());
			control.setLoadCallback([this](std::expected<SystemControlDevice::LoadRequest, std::string> request) { seen = std::move(request); });
		}
		~LoadRam() { control.detachFrom(vm.io()); }

		void text(u32 address, std::string_view value)
		{
			vm.memory().writeBytesUnchecked(Address(address), std::span<const u8>(reinterpret_cast<const u8*>(value.data()), value.size()));
			vm.memory().writeUnchecked<u8>(Address(address + static_cast<u32>(value.size())), 0);
		}
		void word(u32 address, u32 value) { vm.memory().writeUnchecked<u32>(Address(address), value); }
		void load(u32 path, u32 arguments)
		{
			control.write(SystemControlDevice::LoadPathRegister, path);
			control.write(SystemControlDevice::LoadArgsRegister, arguments);
			control.write(SystemControlDevice::CommandRegister, SystemControlDevice::CommandLoad);
		}
	};
}

TEST(system_control, command_3_reads_the_path_the_arguments_and_the_environment_from_ram)
{
	LoadRam ram;
	ram.text(0x10000, "games/snake.cres");
	ram.text(0x10020, "snake");
	ram.text(0x10028, "-fast");
	ram.text(0x10030, "PWD=games");
	ram.word(0x10100, 0x10020);   // argv
	ram.word(0x10104, 0x10028);
	ram.word(0x10110, 0x10030);   // envp
	ram.word(0x10114, 0);
	ram.word(0x10200, 2);         // the block: argc, argv, envp
	ram.word(0x10204, 0x10100);
	ram.word(0x10208, 0x10110);
	ram.load(0x10000, 0x10200);

	CHECK(ram.seen.has_value() && ram.seen->has_value());
	if (!ram.seen || !*ram.seen)
		return;
	const SystemControlDevice::LoadRequest& request = **ram.seen;
	CHECK_EQ(request.path, std::string("games/snake.cres"));
	CHECK(request.arguments == std::vector<std::string>({ "snake", "-fast" }));
	CHECK(request.environment == std::optional<std::vector<std::string>>(std::vector<std::string>{ "PWD=games" }));

	// No envp keeps the running program's environment; no block at all makes argv the path alone.
	ram.word(0x10208, 0);
	ram.load(0x10000, 0x10200);
	CHECK(ram.seen->has_value() && !(*ram.seen)->environment.has_value());
	ram.load(0x10000, 0);
	CHECK(ram.seen->has_value() && (*ram.seen)->arguments == std::vector<std::string>({ "games/snake.cres" }));
}

TEST(system_control, command_3_says_why_its_registers_make_no_request)
{
	LoadRam ram;
	ram.text(0x10000, "prog.cres");
	ram.load(0x100, 0);                  // the null page is not the program's RAM
	CHECK(ram.seen.has_value() && !ram.seen->has_value());
	ram.word(0x10200, 1);
	ram.word(0x10204, 0x10301);          // argv is not aligned
	ram.word(0x10208, 0);
	ram.load(0x10000, 0x10200);
	CHECK(!ram.seen->has_value());
	ram.word(0x10200, SystemControlDevice::MaxLoadStrings + 1);
	ram.load(0x10000, 0x10200);
	CHECK(!ram.seen->has_value() && ram.seen->error().find("more than") != std::string::npos);

	// A reset forgets both registers: the next command 3 has no path.
	ram.control.write(SystemControlDevice::LoadPathRegister, 0x10000);
	ram.control.reset();
	ram.control.write(SystemControlDevice::CommandRegister, SystemControlDevice::CommandLoad);
	CHECK(!ram.seen->has_value());

	// Without a callback, command 3 does nothing at all.
	SystemControlDevice alone{};
	alone.write(SystemControlDevice::CommandRegister, SystemControlDevice::CommandLoad);
	CHECK_EQ(alone.exitCode(), u8{ 0 });
}

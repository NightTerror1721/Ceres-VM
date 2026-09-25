// The disk (DiskDevice): the device on its own, and a program reaching it through its registers.
#include "device_test_machine.h"

TEST(disk, a_sector_written_to_the_disk_comes_back_the_same)
{
	Machine m{
		LoadBase(DiskBase), LoadBaseLow(DiskBase),
		Instruction::LI(1, 1),
		Instruction::STR(Base, 1, Off(DiskDevice::SectorRegister)),
		Instruction::LI(2, static_cast<u16>(SourceBuffer)),
		Instruction::LI(3, 8),
		Instruction::STR(Base, 2, Off(DiskDevice::BlockAddressRegister)),
		Instruction::STR(Base, 3, Off(DiskDevice::BlockLengthRegister)),
		Instruction::LI(5, DiskDevice::BlockCommandWrite),
		Instruction::STR(Base, 5, Off(DiskDevice::BlockCommandRegister)), // memory -> sector 1
		Instruction::LI(4, static_cast<u16>(DestinationBuffer)),
		Instruction::STR(Base, 4, Off(DiskDevice::BlockAddressRegister)),
		Instruction::LI(5, DiskDevice::BlockCommandRead),
		Instruction::STR(Base, 5, Off(DiskDevice::BlockCommandRegister)), // sector 1 -> memory, somewhere else
	};

	DiskDevice disk{};
	disk.attachTo(m.vm().io());

	fill(m.memory(), SourceBuffer, "ON A DISK");
	m.step(16);

	CHECK_EQ(readBack(m.memory(), DestinationBuffer, 8), std::string{ "ON A DIS" });

	// And the sector it did not select is untouched, which is the whole reason for selecting one.
	CHECK_EQ(disk.image()[0], u8{ 0 });
}

TEST(disk, a_sector_past_the_end_of_the_disk_is_refused_rather_than_wrapped)
{
	Machine m{
		LoadBase(DiskBase), LoadBaseLow(DiskBase),
		Instruction::LI(1, 9999),
		Instruction::STR(Base, 1, Off(DiskDevice::SectorRegister)),
		Instruction::LI(2, static_cast<u16>(SourceBuffer)),
		Instruction::LI(3, 8),
		Instruction::STR(Base, 2, Off(DiskDevice::BlockAddressRegister)),
		Instruction::STR(Base, 3, Off(DiskDevice::BlockLengthRegister)),
		Instruction::LI(5, DiskDevice::BlockCommandWrite),
		Instruction::STR(Base, 5, Off(DiskDevice::BlockCommandRegister)),
		Instruction::LDR(6, Base, Off(DiskDevice::StatusRegister)),
	};

	DiskDevice disk{ 4 }; // Four sectors, so 9999 is nowhere
	disk.attachTo(m.vm().io());

	fill(m.memory(), SourceBuffer, "NOWHERE");
	m.step(13);

	CHECK_EQ(m.reg(6) & DiskDevice::StatusError, DiskDevice::StatusError);
	CHECK_EQ(m.reg(6) & DiskDevice::StatusReady, 0u);
}

TEST(disk, a_file_backed_disk_still_holds_what_was_written_after_the_machine_stops)
{
	const auto path = std::filesystem::temp_directory_path() / "ceres_test_disk.img";
	std::filesystem::remove(path);

	{
		Machine m{
			LoadBase(DiskBase), LoadBaseLow(DiskBase),
			Instruction::LI(1, 2),
			Instruction::STR(Base, 1, Off(DiskDevice::SectorRegister)),
			Instruction::LI(2, static_cast<u16>(SourceBuffer)),
			Instruction::LI(3, 6),
			Instruction::STR(Base, 2, Off(DiskDevice::BlockAddressRegister)),
			Instruction::STR(Base, 3, Off(DiskDevice::BlockLengthRegister)),
			Instruction::LI(5, DiskDevice::BlockCommandWrite),
			Instruction::STR(Base, 5, Off(DiskDevice::BlockCommandRegister)),
			Instruction::LI(4, DiskDevice::CommandFlush),
			Instruction::STR(Base, 4, Off(DiskDevice::CommandRegister)),
		};

		DiskDevice disk{};
		CHECK(disk.open(path, 8));
		disk.attachTo(m.vm().io());

		fill(m.memory(), SourceBuffer, "PERSIST");
		m.step(14);
	}

	// A second machine, a second device, the same file.
	Machine m{
		LoadBase(DiskBase), LoadBaseLow(DiskBase),
		Instruction::LI(1, 2),
		Instruction::STR(Base, 1, Off(DiskDevice::SectorRegister)),
		Instruction::LI(2, static_cast<u16>(DestinationBuffer)),
		Instruction::LI(3, 6),
		Instruction::STR(Base, 2, Off(DiskDevice::BlockAddressRegister)),
		Instruction::STR(Base, 3, Off(DiskDevice::BlockLengthRegister)),
		Instruction::LI(5, DiskDevice::BlockCommandRead),
		Instruction::STR(Base, 5, Off(DiskDevice::BlockCommandRegister)),
	};

	DiskDevice disk{};
	CHECK(disk.open(path, 8));
	disk.attachTo(m.vm().io());
	m.step(12);

	CHECK_EQ(readBack(m.memory(), DestinationBuffer, 6), std::string{ "PERSIS" });
	std::filesystem::remove(path);
}

TEST(disk, an_empty_disk_image_is_a_new_disk_of_the_asked_size)
{
	// A zero-byte file is a disk nobody has written yet, not a disk of no sectors.
	const auto path = std::filesystem::temp_directory_path() / "ceres_empty_disk.img";
	{ std::ofstream file(path, std::ios::binary | std::ios::trunc); }
	{
		DiskDevice disk{};
		CHECK(disk.open(path, 4));
		CHECK_EQ(disk.read(DiskDevice::SectorCountRegister), 4u);
	}
	std::filesystem::remove(path);
}

TEST(disk, the_disk_reports_how_many_sectors_it_has)
{
	DiskDevice disk{ 10 };
	CHECK_EQ(disk.read(DiskDevice::SectorCountRegister), 10u);

	DiskDevice big{ 1000 };
	CHECK_EQ(big.read(DiskDevice::SectorCountRegister), 1000u);
}

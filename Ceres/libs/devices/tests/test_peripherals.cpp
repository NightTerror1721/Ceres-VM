// The peripheral ports: media that the host plugs in and pulls out while the machine runs, and that a program reads
// through registers. Driven here the way a program drives it - registers in, registers out, RAM on the side - with
// the host's side (attach, detach) called directly, the way a window or a debugger would.

#include "framework.h"
#include <ceres/vm/ceresvm.h>
#include <ceres/devices/storage/peripherals.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::devices;
using namespace ceres::testing;

namespace
{
	using P = PeripheralDevice;

	constexpr u32 Buffer = 0x1000;

	struct Rig
	{
		CeresVM vm{ Memory::DefaultSize };
		PeripheralDevice dev;

		Rig() { dev.attachTo(vm.io()); }

		u32 read(Address reg) { return dev.read(reg); }
		void write(Address reg, u32 value) { dev.write(reg, value); }
		void select(u32 port) { write(P::PortSelectRegister, port); }

		// A transfer of `length` bytes of the selected port's `sector`, between it and RAM at Buffer.
		void transfer(u32 command, u32 sector, u32 length)
		{
			write(P::SectorRegister, sector);
			write(P::BlockAddressRegister, Buffer);
			write(P::BlockLengthRegister, length);
			write(P::BlockCommandRegister, command);
		}

		void put(std::string_view text)
		{
			for (u32 i = 0; i < text.size(); ++i)
				vm.memory().writeUnchecked<u8>(Address(Buffer + i), static_cast<u8>(text[i]));
		}

		std::string get(u32 size)
		{
			std::string out;
			for (u32 i = 0; i < size; ++i)
				out.push_back(static_cast<char>(vm.memory().readUnchecked<u8>(Address(Buffer + i))));
			return out;
		}

		bool interruptPending() { return (vm.interrupts().pendingMask() & (u64{ 1 } << static_cast<u8>(P::Interrupt))) != 0; }
	};

	std::vector<u8> bytesOf(std::string_view text, usize size = 0)
	{
		std::vector<u8> image(text.begin(), text.end());
		if (size != 0)
			image.resize(size, 0);
		return image;
	}

	// A whole file as a string, with the stream closed again before anyone tries to delete the file
	std::string contentsOf(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	}

	std::filesystem::path scratch(const char* name)
	{
		auto path = std::filesystem::temp_directory_path() / name;
		std::error_code ignored;
		std::filesystem::remove(path, ignored);
		return path;
	}
}

TEST(peripherals, a_machine_starts_with_four_empty_ports_and_nothing_to_report)
{
	Rig rig;
	CHECK_EQ(rig.read(P::PortCountRegister), 4u);
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusEventPending, 0u);
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusReady, P::StatusReady);
	CHECK_EQ(rig.read(P::EventRegister), 0u);
	for (u32 port = 0; port < 4; ++port)
	{
		rig.select(port);
		CHECK_EQ(rig.read(P::PortStatusRegister) & P::PortPresent, 0u);
		CHECK_EQ(rig.read(P::PortTypeRegister), P::TypeNone);
		CHECK_EQ(rig.read(P::PortIdRegister), 0u);
		CHECK_EQ(rig.read(P::PortSectorsRegister), 0u);
	}
	CHECK(!rig.interruptPending());
	CHECK_EQ(rig.dev.describe(0), std::string{ "empty" });
}

TEST(peripherals, plugging_something_in_queues_an_event_and_raises_the_interrupt)
{
	Rig rig;
	CHECK(rig.dev.attachImage(2, "stick.img", bytesOf("hello", 1024), P::Kind::Storage));
	CHECK(rig.interruptPending());
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusEventPending, P::StatusEventPending);

	const u32 event = rig.read(P::EventRegister);
	CHECK_EQ(event, P::EventValid | (P::EventConnected << 8) | 2u);
	CHECK_EQ(rig.read(P::EventRegister), 0u);                                      // taken off the queue
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusEventPending, 0u);
}

TEST(peripherals, events_come_out_in_the_order_they_happened)
{
	Rig rig;
	rig.dev.attachImage(0, "a", bytesOf("a", 512), P::Kind::Storage);
	rig.dev.attachImage(3, "b", bytesOf("b", 512), P::Kind::Storage);
	rig.dev.detach(0);
	CHECK_EQ(rig.dev.pendingEvents(), usize{ 3 });
	CHECK_EQ(rig.read(P::EventRegister), P::EventValid | (P::EventConnected << 8) | 0u);
	CHECK_EQ(rig.read(P::EventRegister), P::EventValid | (P::EventConnected << 8) | 3u);
	CHECK_EQ(rig.read(P::EventRegister), P::EventValid | (P::EventDisconnected << 8) | 0u);
	CHECK_EQ(rig.read(P::EventRegister), 0u);
}

TEST(peripherals, a_port_tells_what_it_holds)
{
	Rig rig;
	rig.dev.attachImage(1, "stick.img", bytesOf("x", 2048), P::Kind::Storage);
	rig.dev.attachImage(2, "game.cart", bytesOf("y", 1024), P::Kind::Cartridge);

	rig.select(1);
	CHECK_EQ(rig.read(P::PortStatusRegister) & P::PortPresent, P::PortPresent);
	CHECK_EQ(rig.read(P::PortStatusRegister) & P::PortWriteProtected, 0u);
	CHECK_EQ(rig.read(P::PortTypeRegister), P::TypeStorage);
	CHECK_EQ(rig.read(P::PortSectorsRegister), 4u);
	const u32 stickId = rig.read(P::PortIdRegister);
	CHECK(stickId != 0u);

	rig.select(2);
	CHECK_EQ(rig.read(P::PortTypeRegister), P::TypeCartridge);
	CHECK_EQ(rig.read(P::PortStatusRegister) & P::PortWriteProtected, P::PortWriteProtected);
	CHECK_EQ(rig.read(P::PortSectorsRegister), 2u);
	CHECK(rig.read(P::PortIdRegister) != stickId);                                    // two media, two identifiers

	rig.select(0);
	CHECK_EQ(rig.read(P::PortTypeRegister), P::TypeNone);
	CHECK_EQ(rig.dev.describe(1), std::string{ "storage stick.img (4 sectors)" });
	CHECK_EQ(rig.dev.describe(2), std::string{ "cartridge game.cart (2 sectors, write protected)" });
}

TEST(peripherals, the_same_medium_has_the_same_identifier_every_time_it_is_plugged_in)
{
	Rig rig;
	rig.dev.attachImage(0, "save.img", bytesOf("", 1024), P::Kind::Storage);
	rig.select(0);
	const u32 first = rig.read(P::PortIdRegister);
	rig.dev.detach(0);
	rig.dev.attachImage(3, "save.img", bytesOf("", 1024), P::Kind::Storage);          // another port, the same medium
	rig.select(3);
	CHECK_EQ(rig.read(P::PortIdRegister), first);
}

TEST(peripherals, a_sector_written_to_a_stick_comes_back_the_same)
{
	Rig rig;
	rig.dev.attachImage(0, "stick.img", bytesOf("", 2048), P::Kind::Storage);
	rig.select(0);

	rig.put("ON A STICK");
	rig.transfer(P::BlockCommandWrite, 2, 10);
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, 0u);

	rig.put("XXXXXXXXXX");                                                             // clobber the buffer
	rig.transfer(P::BlockCommandRead, 2, 10);
	CHECK_EQ(rig.get(10), std::string{ "ON A STICK" });

	const std::vector<u8> image = rig.dev.imageOf(0);
	CHECK_EQ(image[2 * 512], u8{ 'O' });
	CHECK_EQ(image[0], u8{ 0 });                                                       // the other sectors are as they were
	CHECK_EQ(image[512], u8{ 0 });
}

TEST(peripherals, a_cartridge_can_be_read_and_never_written)
{
	Rig rig;
	rig.dev.attachImage(1, "game.cart", bytesOf("PROGRAM", 1024), P::Kind::Cartridge);
	rig.select(1);

	rig.transfer(P::BlockCommandRead, 0, 7);
	CHECK_EQ(rig.get(7), std::string{ "PROGRAM" });
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, 0u);

	rig.put("CHANGED");
	rig.transfer(P::BlockCommandWrite, 0, 7);
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, P::StatusError);
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusReady, 0u);
	CHECK_EQ(rig.dev.imageOf(1)[0], u8{ 'P' });                                        // untouched
}

TEST(peripherals, a_stick_can_be_write_protected_by_the_host)
{
	Rig rig;
	rig.dev.attachImage(0, "stick.img", bytesOf("data", 1024), P::Kind::Storage);
	rig.dev.setWriteProtected(0, true);
	rig.select(0);
	CHECK_EQ(rig.read(P::PortStatusRegister) & P::PortWriteProtected, P::PortWriteProtected);

	rig.put("nope");
	rig.transfer(P::BlockCommandWrite, 0, 4);
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, P::StatusError);
	CHECK_EQ(rig.dev.imageOf(0)[0], u8{ 'd' });

	rig.dev.setWriteProtected(0, false);
	rig.transfer(P::BlockCommandWrite, 0, 4);
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, 0u);
	CHECK_EQ(rig.dev.imageOf(0)[0], u8{ 'n' });
}

TEST(peripherals, a_second_medium_in_a_port_is_refused_and_so_is_a_port_that_is_not_there)
{
	Rig rig;
	CHECK(rig.dev.attachImage(0, "first", bytesOf("1", 512), P::Kind::Storage));
	rig.read(P::EventRegister);
	CHECK(!rig.dev.attachImage(0, "second", bytesOf("2", 512), P::Kind::Storage));
	CHECK_EQ(rig.dev.pendingEvents(), usize{ 0 });                                     // no event for a refusal
	CHECK_EQ(rig.dev.imageOf(0)[0], u8{ '1' });                                        // the first is still there
	CHECK(!rig.dev.attachImage(4, "third", bytesOf("3", 512), P::Kind::Storage));
	CHECK(!rig.dev.detach(4));
	CHECK(!rig.dev.detach(2));                                                         // nothing to pull out

	rig.select(1);
	CHECK_EQ(rig.read(P::PortSelectRegister), 1u);
	rig.select(9);                                                                     // no such port: an error, and the selection stays
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, P::StatusError);
	CHECK_EQ(rig.read(P::PortSelectRegister), 1u);
	rig.select(0);
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, 0u);
}

TEST(peripherals, pulling_a_medium_out_tells_the_program_and_empties_the_port)
{
	Rig rig;
	rig.dev.attachImage(1, "stick.img", bytesOf("keep", 512), P::Kind::Storage);
	rig.read(P::EventRegister);
	rig.vm.interrupts().clearAll();

	CHECK(rig.dev.detach(1));
	CHECK(rig.interruptPending());
	CHECK_EQ(rig.read(P::EventRegister), P::EventValid | (P::EventDisconnected << 8) | 1u);
	rig.select(1);
	CHECK_EQ(rig.read(P::PortStatusRegister) & P::PortPresent, 0u);
	CHECK_EQ(rig.read(P::PortSectorsRegister), 0u);

	// A transfer against the empty port fails rather than reading what was there
	rig.transfer(P::BlockCommandRead, 0, 4);
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, P::StatusError);
}

TEST(peripherals, a_program_can_eject_a_medium_itself)
{
	Rig rig;
	rig.dev.attachImage(2, "stick.img", bytesOf("x", 512), P::Kind::Storage);
	rig.read(P::EventRegister);
	rig.vm.interrupts().clearAll();

	rig.select(2);
	rig.write(P::CommandRegister, P::CommandEject);
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, 0u);
	CHECK_EQ(rig.read(P::PortStatusRegister) & P::PortPresent, 0u);
	CHECK(rig.interruptPending());
	CHECK_EQ(rig.read(P::EventRegister), P::EventValid | (P::EventDisconnected << 8) | 2u);

	rig.write(P::CommandRegister, P::CommandEject);                                    // nothing left to eject
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, P::StatusError);
	rig.write(P::CommandRegister, 77);                                                 // not a command
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, P::StatusError);
}

TEST(peripherals, a_transfer_that_does_not_fit_is_refused)
{
	Rig rig;
	rig.dev.attachImage(0, "stick.img", bytesOf("", 1024), P::Kind::Storage);        // two sectors
	rig.select(0);
	rig.put("data");

	rig.transfer(P::BlockCommandWrite, 2, 4);                                          // sector 2 is past the end
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, P::StatusError);
	rig.transfer(P::BlockCommandWrite, 0, 513);                                        // more than a sector
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, P::StatusError);
	rig.transfer(P::BlockCommandRead, 1, 0);                                           // nothing to move is fine
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, 0u);
	rig.write(P::BlockCommandRegister, 9);                                             // not a block command
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, P::StatusError);
}

TEST(peripherals, an_image_that_is_not_a_whole_number_of_sectors_is_padded_with_zeros)
{
	Rig rig;
	rig.dev.attachImage(0, "odd.img", bytesOf("abc", 700), P::Kind::Storage);
	rig.select(0);
	CHECK_EQ(rig.read(P::PortSectorsRegister), 2u);
	CHECK_EQ(rig.dev.imageOf(0).size(), usize{ 1024 });
	rig.transfer(P::BlockCommandRead, 1, 8);
	CHECK_EQ(rig.get(8), std::string(8, '\0'));
}

TEST(peripherals, a_stick_backed_by_a_file_keeps_what_was_written_after_it_is_pulled_out)
{
	const auto path = scratch("ceres_test_stick.img");
	Rig rig;
	std::string error;
	CHECK(rig.dev.attachFile(0, path, P::Kind::Storage, &error));                    // a file that is not there yet is created
	CHECK_EQ(rig.dev.sectorCount(0), 64u);

	rig.select(0);
	rig.put("SAVED");
	rig.transfer(P::BlockCommandWrite, 3, 5);
	CHECK(rig.dev.detach(0));

	const std::string text = contentsOf(path);
	CHECK_EQ(text.size(), usize{ 64 * 512 });
	CHECK_EQ(text.substr(3 * 512, 5), std::string{ "SAVED" });

	// Plugged back in, it is what was left
	CHECK(rig.dev.attachFile(1, path, P::Kind::Storage));
	rig.select(1);
	rig.transfer(P::BlockCommandRead, 3, 5);
	CHECK_EQ(rig.get(5), std::string{ "SAVED" });
	std::filesystem::remove(path);
}

TEST(peripherals, a_flush_command_writes_the_file_without_pulling_the_medium_out)
{
	const auto path = scratch("ceres_test_flush.img");
	Rig rig;
	CHECK(rig.dev.attachFile(0, path, P::Kind::Storage));
	rig.select(0);
	rig.put("NOW");
	rig.transfer(P::BlockCommandWrite, 0, 3);
	rig.write(P::CommandRegister, P::CommandFlush);
	CHECK_EQ(rig.read(P::StatusRegister) & P::StatusError, 0u);

	const std::string text = contentsOf(path);
	CHECK_EQ(text.substr(0, 3), std::string{ "NOW" });
	CHECK(rig.dev.isPresent(0));
	std::filesystem::remove(path);
}

TEST(peripherals, a_file_that_cannot_be_plugged_in_says_why)
{
	Rig rig;
	std::string error;
	CHECK(!rig.dev.attachFile(0, scratch("ceres_no_such.cart"), P::Kind::Cartridge, &error));
	CHECK(error.find("does not exist") != std::string::npos);

	const auto empty = scratch("ceres_empty.cart");
	{ std::ofstream file(empty, std::ios::binary); }
	CHECK(!rig.dev.attachFile(0, empty, P::Kind::Cartridge, &error));
	CHECK(error.find("empty") != std::string::npos);
	std::filesystem::remove(empty);

	CHECK(!rig.dev.attachFile(7, scratch("ceres_x.img"), P::Kind::Storage, &error));
	CHECK(error.find("no port 7") != std::string::npos);

	CHECK(rig.dev.attachFile(0, scratch("ceres_taken.img"), P::Kind::Storage));
	CHECK(!rig.dev.attachFile(0, scratch("ceres_taken2.img"), P::Kind::Storage, &error));
	CHECK(error.find("already") != std::string::npos);
	std::filesystem::remove(scratch("ceres_taken.img"));
}

TEST(peripherals, a_file_goes_to_the_first_free_port)
{
	const auto a = scratch("ceres_free_a.img");
	const auto b = scratch("ceres_free_b.img");
	Rig rig;
	CHECK_EQ(rig.dev.attachToFreePort(a, P::Kind::Storage), 0);
	CHECK_EQ(rig.dev.attachToFreePort(b, P::Kind::Storage), 1);
	rig.dev.detach(0);
	CHECK_EQ(rig.dev.attachToFreePort(b, P::Kind::Storage), 0);                      // the gap comes first
	CHECK_EQ(rig.dev.attachToFreePort(a, P::Kind::Storage), 2);
	CHECK_EQ(rig.dev.attachToFreePort(a, P::Kind::Storage), 3);
	std::string error;
	CHECK_EQ(rig.dev.attachToFreePort(a, P::Kind::Storage, &error), -1);             // all four taken
	CHECK(error.find("every port") != std::string::npos);
	for (u32 port = 0; port < 4; ++port)
		rig.dev.detach(port);
	std::filesystem::remove(a);
	std::filesystem::remove(b);
}

TEST(peripherals, media_still_plugged_in_when_the_machine_ends_keep_their_writes)
{
	const auto path = scratch("ceres_test_end.img");
	{
		Rig rig;
		CHECK(rig.dev.attachFile(0, path, P::Kind::Storage));
		rig.select(0);
		rig.put("BYE");
		rig.transfer(P::BlockCommandWrite, 0, 3);
	}   // the machine is gone, the stick was never pulled out
	const std::string text = contentsOf(path);
	CHECK_EQ(text.substr(0, 3), std::string{ "BYE" });
	std::filesystem::remove(path);
}

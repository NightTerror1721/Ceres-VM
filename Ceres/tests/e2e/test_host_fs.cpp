// The host file device (semihosting): a program opens, writes, reads, lists and removes files of one host
// directory, and cannot name anything outside it. Driven the way a program drives it - registers in,
// registers out, names and data in RAM.

#include "framework.h"
#include <ceres/vm/ceresvm.h>
#include <ceres/devices/storage/host_fs.h>
#include <ceres/devices/devices.h>
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
	using H = HostFsDevice;

	constexpr u32 Name = 0x1000;
	constexpr u32 Other = 0x1200;
	constexpr u32 Data = 0x2000;

	struct Rig
	{
		CeresVM vm{ Memory::DefaultSize };
		HostFsDevice dev;
		std::filesystem::path dir;

		explicit Rig(const char* name)
		{
			dir = std::filesystem::temp_directory_path() / name;
			std::error_code ignored;
			std::filesystem::remove_all(dir, ignored);
			std::filesystem::create_directories(dir / "levels");
			dev.attachTo(vm.io());
		}

		~Rig()
		{
			dev.reset();                              // close the files before removing them
			std::error_code ignored;
			std::filesystem::remove_all(dir, ignored);
		}

		void put(u32 at, std::string_view text)
		{
			for (u32 i = 0; i < text.size(); ++i)
				vm.memory().writeUnchecked<u8>(Address(at + i), static_cast<u8>(text[i]));
			vm.memory().writeUnchecked<u8>(Address(at + static_cast<u32>(text.size())), 0);
		}

		std::string get(u32 at, u32 size)
		{
			std::string out;
			for (u32 i = 0; i < size; ++i)
				out.push_back(static_cast<char>(vm.memory().readUnchecked<u8>(Address(at + i))));
			return out;
		}

		i32 run(u32 command)
		{
			dev.writeWord(H::CommandRegister, command);
			return static_cast<i32>(dev.readUnsignedWord(H::ResultRegister));
		}

		i32 open(std::string_view name, u32 flags)
		{
			put(Name, name);
			dev.writeWord(H::AddressRegister, Name);
			dev.writeWord(H::ArgumentRegister, flags);
			return run(H::CommandOpen);
		}

		i32 onPath(u32 command, std::string_view name)
		{
			put(Name, name);
			dev.writeWord(H::AddressRegister, Name);
			return run(command);
		}

		i32 transfer(u32 command, i32 handle, u32 length)
		{
			dev.writeWord(H::HandleRegister, static_cast<u32>(handle));
			dev.writeWord(H::AddressRegister, Data);
			dev.writeWord(H::LengthRegister, length);
			return run(command);
		}
	};

	std::string contentsOf(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	}
}

TEST(host_fs, without_a_directory_every_operation_is_enodev)
{
	Rig rig{ "ceres_hostfs_none" };
	CHECK_EQ(rig.dev.readUnsignedWord(H::StatusRegister), 0u);
	CHECK_EQ(rig.open("a.txt", H::OpenRead), -H::ErrNoDevice);
}

TEST(host_fs, a_file_is_written_read_back_sought_and_measured)
{
	Rig rig{ "ceres_hostfs_rw" };
	CHECK(rig.dev.setRoot(rig.dir));
	CHECK_EQ(rig.dev.readUnsignedWord(H::StatusRegister), H::StatusAttached);

	const i32 w = rig.open("levels/1.txt", H::OpenWrite | H::OpenCreate | H::OpenTruncate);
	CHECK(w >= 0);
	rig.put(Data, "hello, host");
	CHECK_EQ(rig.transfer(H::CommandWrite, w, 11), 11);
	rig.dev.writeWord(H::HandleRegister, static_cast<u32>(w));
	CHECK_EQ(rig.run(H::CommandClose), 0);
	CHECK_EQ(contentsOf(rig.dir / "levels" / "1.txt"), std::string("hello, host"));

	const i32 r = rig.open("levels/1.txt", H::OpenRead);
	CHECK(r >= 0);
	CHECK_EQ(rig.transfer(H::CommandRead, r, 5), 5);
	CHECK_EQ(rig.get(Data, 5), std::string("hello"));
	rig.dev.writeWord(H::OffsetRegister, static_cast<u32>(-4));
	rig.dev.writeWord(H::ArgumentRegister, 2);                  // from the end
	CHECK_EQ(rig.run(H::CommandSeek), 7);
	CHECK_EQ(rig.transfer(H::CommandRead, r, 100), 4);          // a short read at the end
	CHECK_EQ(rig.get(Data, 4), std::string("host"));
	CHECK_EQ(rig.transfer(H::CommandRead, r, 100), 0);          // and then nothing
	CHECK_EQ(rig.run(H::CommandFileSize), 11);
	CHECK_EQ(rig.onPath(H::CommandStat, "levels/1.txt"), 11);
	CHECK_EQ(rig.onPath(H::CommandStat, "levels"), -H::ErrIsDirectory);
	CHECK_EQ(rig.onPath(H::CommandStat, "missing"), -H::ErrNoEntry);

	const i32 a = rig.open("levels/1.txt", H::OpenWrite | H::OpenAppend);
	CHECK(a >= 0);
	rig.put(Data, "!");
	CHECK_EQ(rig.transfer(H::CommandWrite, a, 1), 1);
	rig.dev.reset();                                            // a reset closes everything
	CHECK_EQ(contentsOf(rig.dir / "levels" / "1.txt"), std::string("hello, host!"));
	CHECK_EQ(rig.transfer(H::CommandRead, r, 1), -H::ErrBadHandle);
}

TEST(host_fs, reads_and_writes_on_one_handle_share_one_position)
{
	// As on a POSIX descriptor: after reading two bytes a write lands at the third, and a read after that
	// write carries on past what it wrote.
	Rig rig{ "ceres_hostfs_shared_position" };
	CHECK(rig.dev.setRoot(rig.dir));
	const i32 f = rig.open("mixed.txt", H::OpenWrite | H::OpenCreate | H::OpenTruncate);
	CHECK(f >= 0);
	rig.put(Data, "abcdef");
	CHECK_EQ(rig.transfer(H::CommandWrite, f, 6), 6);
	rig.dev.writeWord(H::OffsetRegister, 0);
	rig.dev.writeWord(H::ArgumentRegister, 0);                  // from the start
	CHECK_EQ(rig.run(H::CommandSeek), 0);
	CHECK_EQ(rig.transfer(H::CommandRead, f, 2), 2);
	CHECK_EQ(rig.get(Data, 2), std::string("ab"));
	rig.put(Data, "XY");
	CHECK_EQ(rig.transfer(H::CommandWrite, f, 2), 2);
	CHECK_EQ(rig.transfer(H::CommandRead, f, 2), 2);
	CHECK_EQ(rig.get(Data, 2), std::string("ef"));
	rig.dev.writeWord(H::HandleRegister, static_cast<u32>(f));
	CHECK_EQ(rig.run(H::CommandClose), 0);
	CHECK_EQ(contentsOf(rig.dir / "mixed.txt"), std::string("abXYef"));
}

TEST(host_fs, nothing_outside_the_directory_can_be_named)
{
	Rig rig{ "ceres_hostfs_escape" };
	CHECK(rig.dev.setRoot(rig.dir));
	CHECK_EQ(rig.open("../x.txt", H::OpenWrite | H::OpenCreate), -H::ErrInvalid);
	CHECK_EQ(rig.open("levels/../../x.txt", H::OpenWrite | H::OpenCreate), -H::ErrInvalid);
	CHECK_EQ(rig.open("/etc/passwd", H::OpenRead), -H::ErrInvalid);
	CHECK_EQ(rig.open("C:/Windows/win.ini", H::OpenRead), -H::ErrInvalid);
	CHECK_EQ(rig.open("a//b", H::OpenRead), -H::ErrInvalid);
	CHECK_EQ(rig.open("missing.txt", H::OpenRead), -H::ErrNoEntry);
	CHECK_EQ(rig.open("levels", H::OpenRead), -H::ErrIsDirectory);
	CHECK_EQ(rig.open("new.txt", H::OpenRead), -H::ErrNoEntry);   // no create without write
}

TEST(host_fs, a_directory_is_listed_in_order_and_files_are_renamed_and_removed)
{
	Rig rig{ "ceres_hostfs_list" };
	CHECK(rig.dev.setRoot(rig.dir));
	{
		std::ofstream(rig.dir / "b.txt") << "b";
		std::ofstream(rig.dir / "a.txt") << "a";
	}
	CHECK_EQ(rig.onPath(H::CommandMakeDirectory, "saves"), 0);
	CHECK_EQ(rig.onPath(H::CommandMakeDirectory, "saves"), -H::ErrExists);

	std::vector<std::string> names;
	for (u32 i = 0;; ++i)
	{
		rig.put(Name, "");
		rig.dev.writeWord(H::AddressRegister, Name);
		rig.dev.writeWord(H::ArgumentRegister, i);
		rig.dev.writeWord(H::OffsetRegister, Other);
		rig.dev.writeWord(H::LengthRegister, 64);
		const i32 length = rig.run(H::CommandList);
		CHECK(length >= 0);
		if (length <= 0) break;
		names.push_back(rig.get(Other, static_cast<u32>(length)));
	}
	CHECK_EQ(names.size(), usize{ 4 });
	if (names.size() == 4)
	{
		CHECK_EQ(names[0], std::string("a.txt"));
		CHECK_EQ(names[1], std::string("b.txt"));
		CHECK_EQ(names[2], std::string("levels/"));
		CHECK_EQ(names[3], std::string("saves/"));
	}

	rig.put(Name, "a.txt");
	rig.put(Other, "saves/a.txt");
	rig.dev.writeWord(H::AddressRegister, Name);
	rig.dev.writeWord(H::ArgumentRegister, Other);
	CHECK_EQ(rig.run(H::CommandRename), 0);
	CHECK(std::filesystem::exists(rig.dir / "saves" / "a.txt"));
	CHECK_EQ(rig.onPath(H::CommandRemove, "saves"), -H::ErrNotEmpty);
	CHECK_EQ(rig.onPath(H::CommandRemove, "saves/a.txt"), 0);
	CHECK_EQ(rig.onPath(H::CommandRemove, "saves"), 0);
	CHECK_EQ(rig.onPath(H::CommandRemove, "saves"), -H::ErrNoEntry);
	CHECK_EQ(rig.onPath(H::CommandRemove, ""), -H::ErrInvalid);   // never the root itself
}

TEST(host_fs, a_file_opened_for_writing_alone_can_still_be_sought_and_measured)
{
	Rig rig{ "ceres_hostfs_wo" };
	CHECK(rig.dev.setRoot(rig.dir));
	const i32 w = rig.open("out.txt", H::OpenWrite | H::OpenCreate | H::OpenTruncate);
	CHECK(w >= 0);
	rig.put(Data, "0123456789");
	CHECK_EQ(rig.transfer(H::CommandWrite, w, 10), 10);
	CHECK_EQ(rig.run(H::CommandFileSize), 10);
	rig.dev.writeWord(H::OffsetRegister, 0);
	rig.dev.writeWord(H::ArgumentRegister, 2);               // to the end
	CHECK_EQ(rig.run(H::CommandSeek), 10);
	CHECK_EQ(rig.open("no/such/dir/x.txt", H::OpenWrite | H::OpenCreate), -H::ErrNoEntry);
}

TEST(host_fs, a_link_inside_the_directory_does_not_lead_out_of_it)
{
	Rig rig{ "ceres_hostfs_link" };
	CHECK(rig.dev.setRoot(rig.dir));
	const auto outside = std::filesystem::temp_directory_path() / "ceres_hostfs_outside";
	std::error_code error;
	std::filesystem::create_directories(outside, error);
	std::ofstream(outside / "secret.txt") << "secret";
	std::filesystem::create_directory_symlink(outside, rig.dir / "link", error);
	if (error)
	{
		std::filesystem::remove_all(outside, error);
		return;                                             // no symlinks here (Windows without the privilege)
	}
	CHECK_EQ(rig.open("link/secret.txt", H::OpenRead), -H::ErrAccess);
	rig.dev.reset();
	std::filesystem::remove(rig.dir / "link", error);
	std::filesystem::remove_all(outside, error);
}

TEST(host_fs, the_open_files_run_out_at_eight)
{
	Rig rig{ "ceres_hostfs_many" };
	CHECK(rig.dev.setRoot(rig.dir));
	std::ofstream(rig.dir / "f.txt") << "f";
	for (usize i = 0; i < H::MaxOpenFiles; ++i)
		CHECK(rig.open("f.txt", H::OpenRead) >= 0);
	CHECK_EQ(rig.open("f.txt", H::OpenRead), -H::ErrTooManyOpen);
}

TEST(terminal, the_error_stream_is_its_own)
{
	CeresVM vm{ Memory::DefaultSize };
	TerminalDevice terminal;
	terminal.attachTo(vm.io());
	std::string out, err;
	terminal.setOutputSink([&](u8 byte) { out.push_back(static_cast<char>(byte)); });
	terminal.setErrorSink([&](u8 byte) { err.push_back(static_cast<char>(byte)); });
	terminal.writeByte(TerminalDevice::OutputRegister, 'o');
	terminal.writeByte(TerminalDevice::ErrorOutputRegister, 'e');
	const char* text = "oops";
	for (u32 i = 0; i < 4; ++i)
		vm.memory().writeUnchecked<u8>(Address(0x1000 + i), static_cast<u8>(text[i]));
	terminal.writeWord(TerminalDevice::BlockAddressRegister, 0x1000);
	terminal.writeWord(TerminalDevice::BlockLengthRegister, 4);
	terminal.writeWord(TerminalDevice::BlockCommandRegister, TerminalDevice::BlockCommandWriteError);
	CHECK_EQ(out, std::string("o"));
	CHECK_EQ(err, std::string("eoops"));
}

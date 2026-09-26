// The peripheral ports from the command line down to a running program: `--port` and `--cart` on `ceres run`,
// the files they name plugged in before the program starts, and the program reading them through the registers.

#include "framework.h"
#include <ceres/driver/command.h>
#include <ceres/driver/driver.h>
#include <ceres/driver/host_backend.h>
#include <ceres/driver/machine.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace ceres;
using namespace ceres::driver;
using namespace ceres::testing;

namespace
{
	auto parse(std::initializer_list<const char*> words)
	{
		std::vector<std::string> owned(words.begin(), words.end());
		std::vector<char*> argv;
		for (std::string& word : owned)
			argv.push_back(word.data());
		return parseCommandLine(static_cast<int>(argv.size()), argv.data());
	}

	std::filesystem::path write(const char* name, const std::string& bytes)
	{
		const auto path = std::filesystem::temp_directory_path() / name;
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		return path;
	}

	// What a program printed when it was run with these media plugged in, or the diagnostic if it did not start.
	std::string run(const char* name, const std::string& source, std::vector<PortAttachment> ports)
	{
		const auto program = write(name, source);
		std::istringstream input;
		std::ostringstream output;
		std::ostringstream diagnostics;
		RunCommand command{ .input = program };
		command.ports = std::move(ports);
		const int result = execute(command, { &input, &output, &diagnostics });
		std::filesystem::remove(program);
		return result == 0 ? output.str() : "exit " + std::to_string(result) + ": " + diagnostics.str();
	}

	// Prints the number of sectors port 0 reports, as a digit.
	const char* SectorsOfPortZero =
		"@text\n"
		"global main:\n"
		"    la r1, 0xFF0A0000\n"
		"    ldr r2, [r1 + 0x2C]\n"
		"    add r2, r2, 48\n"
		"    la r3, 0xFF000004\n"
		"    str  [r3 + 0], r2\n"
		"    li r0, 1\n"
		"    la r13, 0xFFFF0000\n"
		"    str  [r13 + 0], r0\n"
		"    ret\n";

	// Reads the first byte of sector 0 of port 1 and prints it.
	const char* FirstByteOfPortOne =
		"@data\n"
		"    let buf: u8[8]\n"
		"@text\n"
		"global main:\n"
		"    la r1, 0xFF0A0000\n"
		"    li r2, 1\n"
		"    str [r1 + 0x08], r2\n"
		"    li r2, 0\n"
		"    str [r1 + 0x30], r2\n"
		"    la r3, buf\n"
		"    str [r1 + 0xF0], r3\n"
		"    li r2, 1\n"
		"    str [r1 + 0xF4], r2\n"
		"    str [r1 + 0xF8], r2\n"
		"    ldrb r4, [r3 + 0]\n"
		"    la r5, 0xFF000004\n"
		"    str  [r5 + 0], r4\n"
		"    li r0, 1\n"
		"    la r13, 0xFFFF0000\n"
		"    str  [r13 + 0], r0\n"
		"    ret\n";
}

TEST(driver_ports, run_takes_port_and_cart_options_that_name_a_port_and_a_file)
{
	auto parsed = parse({ "ceres", "run", "demo.casm", "--port", "0=stick.img", "--cart", "1=game.cart", "--port", "3=C:\\images\\a=b.img" });
	CHECK(parsed.has_value());
	if (!parsed) return;
	const auto* command = std::get_if<RunCommand>(&*parsed);
	CHECK(command != nullptr);
	if (!command) return;

	CHECK_EQ(command->ports.size(), usize{ 3 });
	CHECK_EQ(command->ports[0].port, 0u);
	CHECK_EQ(command->ports[0].path.string(), std::string{ "stick.img" });
	CHECK(!command->ports[0].cartridge);
	CHECK_EQ(command->ports[1].port, 1u);
	CHECK(command->ports[1].cartridge);
	CHECK_EQ(command->ports[2].port, 3u);
	CHECK_EQ(command->ports[2].path.string(), std::string{ "C:\\images\\a=b.img" });   // everything after the first '='
}

TEST(driver_ports, a_port_option_has_to_be_number_equals_file)
{
	CHECK(!parse({ "ceres", "run", "demo.casm", "--port", "stick.img" }).has_value());
	CHECK(!parse({ "ceres", "run", "demo.casm", "--port", "=stick.img" }).has_value());
	CHECK(!parse({ "ceres", "run", "demo.casm", "--port", "x=stick.img" }).has_value());
	CHECK(!parse({ "ceres", "run", "demo.casm", "--port", "0=" }).has_value());
	CHECK(!parse({ "ceres", "run", "demo.casm", "--cart" }).has_value());
	CHECK(!parse({ "ceres", "run", "demo.casm", "--cart", "1234=g.cart" }).has_value());
}

TEST(driver_ports, only_run_takes_ports)
{
	CHECK(!parse({ "ceres", "asm", "demo.casm", "--port", "0=stick.img" }).has_value());
	CHECK(!parse({ "ceres", "profile", "demo.casm", "--cart", "0=g.cart" }).has_value());
	CHECK(!parse({ "ceres", "disasm", "demo.casm", "--port", "0=stick.img" }).has_value());
}

TEST(driver_ports, a_program_sees_the_medium_that_was_plugged_in_for_it)
{
	const auto stick = write("ceres_ports_stick.img", std::string(3 * 512, 'x'));
	CHECK_EQ(run("ceres_ports_a.casm", SectorsOfPortZero, { PortAttachment{ 0, stick, false } }), std::string{ "3" });
	// The same program with nothing plugged in reads an empty port
	CHECK_EQ(run("ceres_ports_b.casm", SectorsOfPortZero, {}), std::string{ "0" });
	std::filesystem::remove(stick);
}

TEST(driver_ports, a_cartridge_is_read_through_the_block_registers)
{
	std::string game = "QUEST";
	game.resize(1024, '\0');
	const auto cart = write("ceres_ports_game.cart", game);
	CHECK_EQ(run("ceres_ports_c.casm", FirstByteOfPortOne, { PortAttachment{ 1, cart, true } }), std::string{ "Q" });
	std::filesystem::remove(cart);
}

TEST(driver_ports, a_medium_that_cannot_be_plugged_in_stops_the_run_and_says_why)
{
	const std::string missing = run("ceres_ports_d.casm", SectorsOfPortZero,
		{ PortAttachment{ 1, std::filesystem::temp_directory_path() / "ceres_no_such_game.cart", true } });
	CHECK(missing.starts_with("exit 1"));
	CHECK(missing.find("does not exist") != std::string::npos);

	const std::string badPort = run("ceres_ports_e.casm", SectorsOfPortZero,
		{ PortAttachment{ 9, std::filesystem::temp_directory_path() / "ceres_ports_x.img", false } });
	CHECK(badPort.starts_with("exit 1"));
	CHECK(badPort.find("no port 9") != std::string::npos);
	std::filesystem::remove(std::filesystem::temp_directory_path() / "ceres_ports_x.img");
}

TEST(driver_ports, a_machine_can_have_media_plugged_in_and_pulled_out_while_it_exists)
{
	Machine machine{ MachineConfig{} };
	const auto stick = write("ceres_ports_live.img", std::string(512, 'y'));

	CHECK_EQ(machine.describePeripheral(2), std::string{ "empty" });
	std::string error;
	CHECK(machine.attachPeripheral(2, stick, false, &error));
	CHECK_EQ(machine.describePeripheral(2), std::string{ "storage ceres_ports_live.img (1 sectors)" });
	CHECK(!machine.attachPeripheral(2, stick, false, &error));                     // taken
	CHECK(error.find("already") != std::string::npos);
	CHECK(machine.detachPeripheral(2));
	CHECK(!machine.detachPeripheral(2));
	CHECK_EQ(machine.describePeripheral(2), std::string{ "empty" });
	std::filesystem::remove(stick);
}

namespace
{
	// A host with a window that a file gets dropped on, on its second visit
	class DropBackend final : public HostBackend
	{
	public:
		std::filesystem::path dropped;
		std::function<void(const std::filesystem::path&)> handler;
		int pumps = 0;

		bool pump(InputSink&) override
		{
			if (++pumps == 2 && handler)
				handler(dropped);
			return true;
		}
		void present(const devices::DisplayDevice&) override {}
		void setFileDropHandler(std::function<void(const std::filesystem::path&)> h) override { handler = std::move(h); }
	};

	// Waits for an event, then prints the port it names and how many sectors that port holds
	const char* WaitForAMedium =
		"@text\n"
		"global main:\n"
		"    la r1, 0xFF0A0000\n"
		".wait:\n"
		"    ldr r2, [r1 + 0x00]\n"
		"    and r2, r2, 1\n"
		"    cmp r2, 1\n"
		"    jnz .wait\n"
		"    ldr r2, [r1 + 0x0C]\n"
		"    and r6, r2, 255\n"
		"    str [r1 + 0x08], r6\n"      // look at the port the event names
		"    ldr r3, [r1 + 0x2C]\n"
		"    add r3, r3, 48\n"
		"    add r6, r6, 48\n"
		"    la r4, 0xFF000004\n"
		"    str  [r4 + 0], r6\n"
		"    str  [r4 + 0], r3\n"
		"    li r0, 1\n"
		"    la r13, 0xFFFF0000\n"
		"    str  [r13 + 0], r0\n"
		"    ret\n";
}

TEST(driver_ports, a_file_dropped_on_the_window_is_plugged_into_the_first_free_port_and_the_program_is_told)
{
	const auto stick = write("ceres_ports_dropped.img", std::string(2 * 512, 'z'));
	const auto program = write("ceres_ports_drop.casm", WaitForAMedium);
	auto* raw = new DropBackend;
	raw->dropped = stick;
	std::unique_ptr<HostBackend> owned(raw);
	const HostBackendFactory factory = [&]() -> std::unique_ptr<HostBackend> { return std::move(owned); };

	std::istringstream input;
	std::ostringstream output;
	std::ostringstream diagnostics;
	const int result = execute(RunCommand{ .input = program, .window = true }, { &input, &output, &diagnostics }, factory);
	std::filesystem::remove(program);
	std::filesystem::remove(stick);

	CHECK_EQ(result, 0);
	CHECK_EQ(output.str(), std::string{ "02" });     // an event for port 0, which holds two sectors
	CHECK_EQ(diagnostics.str(), std::string{});
}

// Stamped input (plan/v2 SPEC 3.3): the host's input goes in between slices with its cycle, and a recording of it
// replays the same run.
#include "framework.h"
#include <ceres/driver/command.h>
#include <ceres/driver/driver.h>
#include <ceres/driver/input_journal.h>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace ceres;
using namespace ceres::driver;

namespace
{
	struct Devices
	{
		devices::TerminalDevice terminal;
		devices::KeyboardDevice keyboard;
		devices::MouseDevice mouse;
		devices::GamepadDevice gamepad;
		std::vector<std::filesystem::path> dropped;
		InputTargets targets{ terminal, keyboard, mouse, gamepad, [this](const std::filesystem::path& path) { dropped.push_back(path); } };
	};

	std::string readAll(devices::TerminalDevice& terminal)
	{
		std::string out;
		while (terminal.availableBytes() > 0)
			out += static_cast<char>(terminal.read(devices::TerminalDevice::InputRegister));
		return out;
	}
}

TEST(input_journal, every_kind_of_event_survives_a_line_of_the_recording)
{
	std::vector<InputEvent> events;
	events.push_back(InputEvent{ .cycle = 12, .kind = InputEvent::Kind::TerminalBytes, .data = std::string("hi\n\0\xFF", 5) });
	events.push_back(InputEvent{ .cycle = 13, .kind = InputEvent::Kind::TerminalClose });
	InputEvent key{ .cycle = 14, .kind = InputEvent::Kind::Key };
	key.values = { 40, 1 };
	events.push_back(key);
	events.push_back(InputEvent{ .cycle = 15, .kind = InputEvent::Kind::Text, .data = "\xC3\xA9" });
	InputEvent mouse{ .cycle = 16, .kind = InputEvent::Kind::Mouse };
	mouse.values = { -3, 7, 1, -1 };
	events.push_back(mouse);
	InputEvent pad{ .cycle = 17, .kind = InputEvent::Kind::Gamepad };
	pad.values = { 5, -32768, 32767, 0, 1, 255, 0 };
	events.push_back(pad);
	events.push_back(InputEvent{ .cycle = 18, .kind = InputEvent::Kind::FileDrop, .data = "C:/a b/c.cart" });
	events.push_back(InputEvent{ .cycle = 19, .kind = InputEvent::Kind::Quit });
	events.push_back(InputEvent{ .cycle = 0, .kind = InputEvent::Kind::Reset });

	for (const InputEvent& event : events)
	{
		const auto back = parseInputEvent(formatInputEvent(event));
		CHECK(back.has_value() && *back == event);
	}
	CHECK_EQ(formatInputEvent(events[0]), std::string("12 term 68690a00ff"));
}

TEST(input_journal, a_line_that_is_not_an_event_is_refused)
{
	for (const char* wrong : { "", "x term 00", "12", "12 nothing", "12 term 0", "12 term zz", "12 key 1", "12 key 1 2 3", "12 quit extra" })
		CHECK(!parseInputEvent(wrong).has_value());

	std::istringstream noHeader("12 quit\n");
	CHECK(!readInputRecording(noHeader).has_value());
	std::istringstream badLine("ceres-input 1\n12 quit\nnonsense\n");
	const auto bad = readInputRecording(badLine);
	CHECK(!bad.has_value() && bad.error().find("line 3") != std::string::npos);
	std::istringstream good("ceres-input 1\r\n12 quit\r\n\r\n");
	const auto read = readInputRecording(good);
	CHECK(read.has_value() && read->size() == 1);
}

TEST(input_journal, terminal_bytes_go_in_as_far_as_the_ring_has_room)
{
	Devices d;
	InputHub hub;
	std::ostringstream recording;
	hub.record(recording);
	const std::string text(100, 'x');
	hub.post(InputEvent{ .kind = InputEvent::Kind::TerminalBytes, .data = text });
	hub.post(InputEvent{ .kind = InputEvent::Kind::TerminalClose });

	CHECK(hub.inject(10, d.targets));
	CHECK_EQ(d.terminal.availableBytes(), devices::TerminalDevice::InputBufferCapacity - 1);
	CHECK(!d.terminal.isInputClosed());         // the close waits behind the bytes still to go
	CHECK_EQ(readAll(d.terminal), std::string(63, 'x'));

	CHECK(hub.inject(20, d.targets));
	CHECK_EQ(readAll(d.terminal), std::string(37, 'x'));
	CHECK(d.terminal.isInputClosed());

	// What went in, and when: the recording holds what the terminal took, not what was posted.
	std::istringstream in(recording.str());
	const auto events = readInputRecording(in);
	CHECK(events.has_value() && events->size() == 3);
	if (events && events->size() == 3)
	{
		CHECK_EQ((*events)[0].cycle, u64{ 10 });
		CHECK_EQ((*events)[0].data.size(), usize{ 63 });
		CHECK_EQ((*events)[1].cycle, u64{ 20 });
		CHECK((*events)[2].kind == InputEvent::Kind::TerminalClose);
	}
}

TEST(input_journal, a_replay_injects_each_event_at_its_cycle_and_ignores_the_host)
{
	Devices d;
	InputHub hub;
	hub.replay({
		InputEvent{ .cycle = 100, .kind = InputEvent::Kind::TerminalBytes, .data = "a" },
		InputEvent{ .cycle = 200, .kind = InputEvent::Kind::FileDrop, .data = "stick.img" },
		InputEvent{ .cycle = 300, .kind = InputEvent::Kind::Quit },
	});
	hub.post(InputEvent{ .kind = InputEvent::Kind::TerminalBytes, .data = "host" });   // ignored

	CHECK(hub.inject(99, d.targets));
	CHECK_EQ(d.terminal.availableBytes(), usize{ 0 });
	CHECK(hub.inject(100, d.targets));
	CHECK_EQ(readAll(d.terminal), std::string("a"));
	CHECK(hub.inject(250, d.targets));
	CHECK(d.dropped.size() == 1 && d.dropped[0] == std::filesystem::path("stick.img"));
	CHECK(!hub.inject(300, d.targets));         // where the recording's host quit
}

TEST(input_journal, a_replay_waits_for_the_restart_its_recording_saw)
{
	Devices d;
	InputHub hub;
	hub.replay({
		InputEvent{ .cycle = 50, .kind = InputEvent::Kind::TerminalBytes, .data = "1" },
		InputEvent{ .cycle = 0, .kind = InputEvent::Kind::Reset },
		InputEvent{ .cycle = 10, .kind = InputEvent::Kind::TerminalBytes, .data = "2" },
	});
	CHECK(hub.inject(60, d.targets));
	CHECK_EQ(readAll(d.terminal), std::string("1"));   // "2" counts from after the restart, not from here
	hub.restarted();
	CHECK(hub.inject(5, d.targets));
	CHECK_EQ(d.terminal.availableBytes(), usize{ 0 });
	CHECK(hub.inject(10, d.targets));
	CHECK_EQ(readAll(d.terminal), std::string("2"));
}

TEST(input_journal, a_recorded_run_replays_the_same_way)
{
	// Echoes its input and exits, at the end of it, with the low byte of the cycle count: that depends on the
	// cycle each byte went in at, which a live run takes from the host's timing and a replay from the recording.
	const auto source = std::filesystem::temp_directory_path() / "ceres_input_journal_test.casm";
	const auto recording = std::filesystem::temp_directory_path() / "ceres_input_journal_test.input";
	{
		std::ofstream file{ source };
		file << "@text\n"
			"global main:\n"
			"    la   r13, 0xFF000000\n"
			".loop:\n"
			"    ldr  r1, [r13 + 0]\n"
			"    and  r2, r1, 1\n"
			"    cmp  r2, 0\n"
			"    jnz  .take\n"
			"    and  r2, r1, 4\n"
			"    cmp  r2, 0\n"
			"    jnz  .done\n"
			"    halt\n"
			"    jp   .loop\n"
			".take:\n"
			"    ldr  r3, [r13 + 8]\n"
			"    str  [r13 + 4], r3\n"
			"    jp   .loop\n"
			".done:\n"
			"    la   r12, 0xFF010000\n"
			"    ldr  r4, [r12 + 0]\n"
			"    and  r4, r4, 255\n"
			"    shl  r4, r4, 8\n"
			"    or   r4, r4, 1\n"
			"    la   r12, 0xFFFF0000\n"
			"    str  [r12 + 0], r4\n";
	}

	const auto run = [&](std::string typed, RunCommand command)
	{
		std::istringstream input(std::move(typed));
		std::ostringstream output;
		std::ostringstream diagnostics;
		command.input = source;
		const int status = execute(command, { &input, &output, &diagnostics });
		return std::pair{ status, output.str() };
	};

	RunCommand record;
	record.record = recording;
	const auto live = run(std::string(200, 'q') + "end\n", record);
	RunCommand replay;
	replay.replay = recording;
	const auto again = run("something else entirely", replay);

	std::filesystem::remove(source);
	std::filesystem::remove(recording);
	CHECK_EQ(live.second, std::string(200, 'q') + "end\n");
	CHECK_EQ(again.second, live.second);
	CHECK_EQ(again.first, live.first);
}

TEST(driver_command, record_and_replay_belong_to_run_and_exclude_each_other)
{
	char program[] = "ceres";
	char run[] = "run";
	char profile[] = "profile";
	char input[] = "main.casm";
	char record[] = "--record";
	char replay[] = "--replay";
	char file[] = "session.input";
	char* runArgv[] = { program, run, input, record, file };
	auto parsed = parseCommandLine(5, runArgv);
	const auto* command = parsed ? std::get_if<RunCommand>(&*parsed) : nullptr;
	CHECK(command != nullptr && command->record == std::filesystem::path("session.input"));

	char* both[] = { program, run, input, record, file, replay, file };
	CHECK(!parseCommandLine(7, both).has_value());
	char* profileArgv[] = { program, profile, input, replay, file };
	CHECK(!parseCommandLine(5, profileArgv).has_value());
}

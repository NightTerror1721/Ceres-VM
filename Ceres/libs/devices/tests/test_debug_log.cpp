// The debug log (DebugLogDevice): the device on its own, and a program reaching it through its registers.
#include "device_test_machine.h"

namespace
{
	struct Collected
	{
		std::vector<std::pair<u32, std::string>> lines;
		int breaks = 0;
	};

	void writeText(DebugLogDevice& log, std::string_view text)
	{
		for (const char c : text)
			log.write(DebugLogDevice::OutputRegister, static_cast<u8>(c));
	}
}

TEST(debug_log, a_line_goes_to_the_host_with_its_level_when_it_ends)
{
	DebugLogDevice log{};
	Collected got;
	CHECK_EQ(log.read(DebugLogDevice::EnabledRegister), 0u);   // nobody collects it yet
	log.setSink([&](u32 level, std::string_view line) { got.lines.emplace_back(level, std::string(line)); });
	CHECK_EQ(log.read(DebugLogDevice::EnabledRegister), 1u);
	CHECK_EQ(log.read(DebugLogDevice::LevelRegister), DebugLogDevice::LevelInfo);

	writeText(log, "hello");
	CHECK(got.lines.empty());                                 // not until the newline
	writeText(log, "\n");
	log.write(DebugLogDevice::LevelRegister, DebugLogDevice::LevelError);
	writeText(log, "bad\n");
	log.write(DebugLogDevice::LevelRegister, 9);              // past debug: taken as debug
	CHECK_EQ(log.read(DebugLogDevice::LevelRegister), DebugLogDevice::LevelDebug);
	writeText(log, "half");
	log.write(DebugLogDevice::FlushRegister, 1);
	log.write(DebugLogDevice::FlushRegister, 1);              // nothing left: nothing sent
	log.write(DebugLogDevice::OutputRegister, 0x4141u);        // only the low byte counts
	log.write(DebugLogDevice::OutputRegister, '\n');

	CHECK_EQ(got.lines.size(), usize{ 4 });
	if (got.lines.size() == 4)
	{
		CHECK_EQ(got.lines[0].first, DebugLogDevice::LevelInfo);
		CHECK_EQ(got.lines[0].second, std::string("hello"));
		CHECK_EQ(got.lines[1].first, DebugLogDevice::LevelError);
		CHECK_EQ(got.lines[1].second, std::string("bad"));
		CHECK_EQ(got.lines[2].first, DebugLogDevice::LevelDebug);
		CHECK_EQ(got.lines[2].second, std::string("half"));
		CHECK_EQ(got.lines[3].second, std::string("A"));
	}
	CHECK_EQ(DebugLogDevice::levelName(1), std::string_view("warn"));
}

TEST(debug_log, a_line_with_no_end_is_sent_in_pieces_and_a_reset_drops_it)
{
	DebugLogDevice log{};
	std::vector<usize> sizes;
	log.setSink([&](u32, std::string_view line) { sizes.push_back(line.size()); });
	writeText(log, std::string(DebugLogDevice::MaxLine + 10, 'x'));
	CHECK_EQ(sizes.size(), usize{ 1 });
	if (!sizes.empty())
		CHECK_EQ(sizes[0], DebugLogDevice::MaxLine);

	log.write(DebugLogDevice::LevelRegister, DebugLogDevice::LevelError);
	log.reset();
	log.write(DebugLogDevice::FlushRegister, 1);
	CHECK_EQ(sizes.size(), usize{ 1 });                        // the ten left over went with the reset
	CHECK_EQ(log.read(DebugLogDevice::LevelRegister), DebugLogDevice::LevelInfo);
}

TEST(debug_log, a_program_logs_a_line_and_asks_to_break)
{
	// Break does nothing without a handler, and calls it with one: the debugger stops there.
	Machine m{
		LoadBase(default_mmio::DebugLog), LoadBaseLow(default_mmio::DebugLog),
		Instruction::LI(1, 'o'),
		Instruction::STR(Base, 1, Off(DebugLogDevice::OutputRegister)),
		Instruction::LI(1, 'k'),
		Instruction::STR(Base, 1, Off(DebugLogDevice::OutputRegister)),
		Instruction::LI(1, '\n'),
		Instruction::STR(Base, 1, Off(DebugLogDevice::OutputRegister)),
		Instruction::STR(Base, 1, Off(DebugLogDevice::BreakRegister)),
		Instruction::STR(Base, 1, Off(DebugLogDevice::BreakRegister)),
		Instruction::LDR(2, Base, Off(DebugLogDevice::EnabledRegister)),
	};
	DebugLogDevice log{};
	log.attachTo(m.vm().io());
	Collected got;
	log.setSink([&](u32 level, std::string_view line) { got.lines.emplace_back(level, std::string(line)); });

	m.step(9);
	log.setBreakHandler([&] { ++got.breaks; });
	m.step(2);

	CHECK_EQ(got.lines.size(), usize{ 1 });
	if (!got.lines.empty())
		CHECK_EQ(got.lines[0].second, std::string("ok"));
	CHECK_EQ(got.breaks, 1);
	CHECK_EQ(m.reg(2), 1u);
	CHECK(log.registers().find(DebugLogDevice::BreakRegister.value()) != nullptr);
}

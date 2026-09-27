// A run without a window (plan/v2 SPEC 10, F5.7): its output in files, its input scripted.
#include "framework.h"
#include "run_capture.h"
#include "key_script.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace ceres;
using namespace ceres::driver;
using namespace ceres::testing;

namespace
{
	std::filesystem::path writeSource(const std::string& text)
	{
		const auto path = uniqueTempPath("ceres_headless") .replace_extension(".casm");
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << text;
		return path;
	}

	// Echoes every byte of its input until the input ends, then shuts down.
	const char* Echo =
		"@text\n"
		"global main:\n"
		"    la   r13, 0xFF000000\n"
		".wait:\n"
		"    ldr  r1, [r13 + 0]\n"
		"    and  r2, r1, 1\n"
		"    cmp  r2, 0\n"
		"    jz   .end\n"
		"    ldr  r3, [r13 + 8]\n"
		"    str  [r13 + 4], r3\n"
		"    jp   .wait\n"
		".end:\n"
		"    and  r2, r1, 4\n"
		"    cmp  r2, 0\n"
		"    jz   .wait\n"
		"    la   r7, 0xFFFF0000\n"
		"    li   r0, 1\n"
		"    str  [r7 + 0], r0\n";
}

TEST(headless, the_screen_log_has_the_screen_at_every_present_and_at_the_end)
{
	// "hi", a Present, two frames so it is applied, then "!" and the end.
	const auto source = writeSource(
		"@text\n"
		"global main:\n"
		"    la   r13, 0xFF000000\n"
		"    li   r1, 104\n"
		"    str  [r13 + 4], r1\n"
		"    li   r1, 105\n"
		"    str  [r13 + 4], r1\n"
		"    la   r12, 0xFF400000\n"
		"    li   r1, 1\n"
		"    str  [r12 + 0x120], r1\n"
		".wait:\n"
		"    ldr  r1, [r12 + 0x118]\n"
		"    cmp  r1, 2\n"
		"    jnz  .wait\n"
		"    li   r1, 33\n"
		"    str  [r13 + 4], r1\n"
		"    la   r7, 0xFFFF0000\n"
		"    li   r0, 1\n"
		"    str  [r7 + 0], r0\n");
	const auto log = uniqueTempPath("ceres_screen_log");
	RunCommand command{ .input = source, .headless = true };
	command.screenLog = log;
	const CapturedRun run = captureRun(command);
	const std::string text = readWhole(log);
	std::filesystem::remove(source);
	std::filesystem::remove(log);

	CHECK_EQ(run.status, 0);
	CHECK_EQ(run.output, std::string("hi!"));
	CHECK(text.starts_with("--- present 1 ---\nhi\n"));
	CHECK(text.find("--- end ---\nhi!\n") != std::string::npos);
	// 30 rows each time: the whole 80 x 30 screen.
	CHECK_EQ(std::count(text.begin(), text.end(), '\n'), 62);
}

TEST(headless, keys_type_at_their_instants_and_the_input_ends_after_the_last)
{
	const auto source = writeSource(Echo);
	const auto keys = uniqueTempPath("ceres_keys");
	{
		std::ofstream file(keys, std::ios::binary);
		file << "# a line, typed a word at a time\n"
			"5 text hello\n"
			"12 press Space\n"
			"12 text world\n"
			"20 press Return\n";
	}
	RunCommand command{ .input = source, .headless = true };
	command.keysFile = keys;
	const CapturedRun run = captureRun(command);
	std::filesystem::remove(source);
	std::filesystem::remove(keys);

	CHECK_EQ(run.status, 0);
	// The space came as a key with no text: named keys the discipline does not use are dropped, so it is not there.
	CHECK_EQ(run.output, std::string("helloworld\n"));
}

TEST(headless, without_anything_to_type_the_input_ends_at_once)
{
	const auto source = writeSource(Echo);
	const CapturedRun run = captureRun(RunCommand{ .input = source, .headless = true });
	std::filesystem::remove(source);
	CHECK_EQ(run.status, 0);
	CHECK(run.output.empty());
}

TEST(headless, a_bad_key_script_is_an_error_that_names_its_line)
{
	const auto source = writeSource(Echo);
	const auto keys = uniqueTempPath("ceres_bad_keys");
	{
		std::ofstream file(keys, std::ios::binary);
		file << "1 press Return\n2 press NoSuchKey\n";
	}
	RunCommand command{ .input = source, .headless = true };
	command.keysFile = keys;
	const CapturedRun run = captureRun(command);
	std::filesystem::remove(source);
	std::filesystem::remove(keys);
	CHECK_EQ(run.status, 1);
	CHECK(run.diagnostics.find("line 2") != std::string::npos);
}

TEST(key_script, events_are_stamped_with_their_cycles_in_time_order)
{
	const auto events = parseKeyScript("10 down a\n2 up LeftShift\n10 press f12\n1000 text \xC3\xA9\n", 50'000'000);
	CHECK(events.has_value());
	if (!events)
		return;
	CHECK_EQ(events->size(), usize{ 5 });
	CHECK_EQ((*events)[0].cycle, u64{ 100'000 });                    // 2 ms at 50 MHz
	CHECK_EQ((*events)[0].values[0], 225);
	CHECK_EQ((*events)[1].values[0], 4);                              // 'a', as written before f12 at the same instant
	CHECK_EQ((*events)[2].values[0], 69);
	CHECK_EQ((*events)[3].values[1], 0);                              // f12 up, a millisecond later
	CHECK_EQ((*events)[3].cycle, u64{ 550'000 });
	CHECK((*events)[4].kind == InputEvent::Kind::Text && (*events)[4].data == "\xC3\xA9");
	CHECK_EQ((*events)[4].cycle, u64{ 50'000'000 });

	CHECK(!parseKeyScript("x down a\n", 50'000'000).has_value());
	CHECK(!parseKeyScript("1 jump a\n", 50'000'000).has_value());
	CHECK(!parseKeyScript("1 text\n", 50'000'000).has_value());
}

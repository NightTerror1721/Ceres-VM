// The terminal's line discipline (plan/v2 SPEC 8.3), driven by scripted keystrokes.
#include "framework.h"

#include <ceres/devices/input/keyboard.h>
#include <ceres/devices/terminal/line_discipline.h>

#include <string>
#include <vector>

using namespace ceres;
using namespace ceres::devices;
using namespace ceres::devices::term;

namespace
{
	class Recorder final : public LineDiscipline::Output
	{
	public:
		std::string echoed;
		std::vector<std::string> delivered;
		int interrupts = 0;
		int ends = 0;
		void echo(std::string_view bytes) override { echoed += bytes; }
		void deliver(std::string_view bytes) override { delivered.emplace_back(bytes); }
		void interrupt() override { ++interrupts; }
		void endOfInput() override { ++ends; }
	};

	constexpr u32 Named = KeyboardDevice::KeyNamed;

	void typeText(LineDiscipline& d, Recorder& r, std::string_view text)
	{
		for (char c : text)
			d.key(static_cast<u8>(c), r);
	}
}

TEST(line_discipline, a_line_is_echoed_and_handed_over_whole_on_enter)
{
	LineDiscipline d;
	Recorder r;
	typeText(d, r, "hello");
	CHECK(r.delivered.empty());
	CHECK_EQ(d.pendingBytes(), usize{ 5 });
	d.key(Named | scancode::Return, r);
	CHECK_EQ(r.delivered.size(), usize{ 1 });
	CHECK_EQ(r.delivered[0], std::string("hello\n"));
	CHECK_EQ(r.echoed, std::string("hello\n"));
}

TEST(line_discipline, editing_keys_change_the_line_before_it_is_sent)
{
	LineDiscipline d;
	Recorder r;
	typeText(d, r, "helo");
	d.key(Named | scancode::Left, r);
	d.key('l', r);                             // hel|o -> hell|o
	d.key(Named | scancode::Home, r);
	d.key(Named | scancode::Delete, r);        // |ello
	d.key('j', r);                             // j|ello
	d.key(Named | scancode::End, r);
	d.key(Named | scancode::Backspace, r);     // jell
	d.key('y', r);
	d.key('\n', r);
	CHECK_EQ(r.delivered.back(), std::string("jelly\n"));
}

TEST(line_discipline, a_return_a_newline_or_both_end_a_line_once)
{
	LineDiscipline d;
	Recorder r;
	typeText(d, r, "one\r\ntwo\rthree\n\n");
	const std::vector<std::string> expected{ "one\n", "two\n", "three\n", "\n" };
	CHECK(r.delivered == expected);
}

TEST(line_discipline, ctrl_u_empties_the_line)
{
	LineDiscipline d;
	Recorder r;
	typeText(d, r, "wrong\x15right\n");
	CHECK_EQ(r.delivered.back(), std::string("right\n"));
}

TEST(line_discipline, up_and_down_walk_the_history_and_come_back_to_the_draft)
{
	LineDiscipline d;
	Recorder r;
	typeText(d, r, "one\ntwo\ndra");
	d.key(Named | scancode::Up, r);
	d.key(Named | scancode::Up, r);
	d.key(Named | scancode::Up, r);            // no further back than there is
	d.key('\n', r);
	CHECK_EQ(r.delivered.back(), std::string("one\n"));
	typeText(d, r, "dra");
	d.key(Named | scancode::Up, r);
	d.key(Named | scancode::Down, r);
	d.key('\n', r);
	CHECK_EQ(r.delivered.back(), std::string("dra\n"));

	LineDiscipline off;
	off.setHistory(false);
	Recorder r2;
	typeText(off, r2, "x\n");
	off.key(Named | scancode::Up, r2);
	off.key('\n', r2);
	CHECK_EQ(r2.delivered.back(), std::string("\n"));
}

TEST(line_discipline, ctrl_c_drops_the_line_and_interrupts)
{
	LineDiscipline d;
	Recorder r;
	typeText(d, r, "abc\x03");
	CHECK_EQ(r.interrupts, 1);
	CHECK(r.delivered.empty());
	CHECK_EQ(d.pendingBytes(), usize{ 0 });
	CHECK(r.echoed.ends_with("^C\n"));
}

TEST(line_discipline, ctrl_d_ends_the_input_on_an_empty_line_and_sends_it_otherwise)
{
	LineDiscipline d;
	Recorder r;
	typeText(d, r, "part\x04");
	CHECK_EQ(r.delivered.back(), std::string("part"));
	CHECK_EQ(r.ends, 0);
	d.key(0x04, r);
	CHECK_EQ(r.ends, 1);
}

TEST(line_discipline, without_echo_nothing_is_drawn_but_the_line_is_the_same)
{
	LineDiscipline d;
	d.setEcho(false);
	Recorder r;
	typeText(d, r, "secret\n");
	CHECK(r.echoed.empty());
	CHECK_EQ(r.delivered.back(), std::string("secret\n"));
}

TEST(line_discipline, raw_mode_sends_every_key_at_once_as_its_bytes)
{
	LineDiscipline d;
	d.setRaw(true);
	Recorder r;
	d.key('a', r);
	d.key(0x03, r);
	d.key(0xE9, r);
	d.key(Named | scancode::Left, r);
	d.key(Named | scancode::PageDown, r);
	d.key(Named | scancode::Return, r);
	const std::vector<std::string> expected{ "a", "\x03", "\xC3\xA9", "\x1b[D", "\x1b[6~", "\n" };
	CHECK(r.delivered == expected);
	CHECK(r.echoed.empty());
	CHECK_EQ(r.interrupts, 0);
}

TEST(line_discipline, flush_hands_over_what_is_left_and_the_named_keys_it_does_not_use_do_nothing)
{
	LineDiscipline d;
	Recorder r;
	typeText(d, r, "tail");
	d.key(Named | scancode::Escape, r);
	d.key(Named | scancode::PageUp, r);
	d.flush(r);
	CHECK_EQ(r.delivered.back(), std::string("tail"));
	d.flush(r);
	CHECK_EQ(r.delivered.size(), usize{ 1 });
}

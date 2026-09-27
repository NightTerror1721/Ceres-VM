// The terminal's output decoder (plan/v2 SPEC 8.2): one test per kind of thing it finds.
#include "framework.h"

#include <ceres/devices/terminal/ansi_parser.h>

#include <string>
#include <string_view>
#include <vector>

using namespace ceres;
using namespace ceres::devices::term;

namespace
{
	// What the parser found, as a string: characters as themselves (or U+xxxx), <ctrl n>, [params private final], <esc x>.
	class Recorder final : public AnsiParser::Handler
	{
	public:
		std::string log;
		void print(u32 cp) override { log += cp < 0x80 ? std::string(1, static_cast<char>(cp)) : "U+" + std::to_string(cp); }
		void control(u8 code) override { log += "<ctrl " + std::to_string(code) + ">"; }
		void csi(char final, std::span<const u32> params, char marker) override
		{
			log += "[";
			if (marker)
				log += marker;
			for (usize i = 0; i < params.size(); ++i)
				log += (i ? ";" : "") + std::to_string(params[i]);
			log += final;
			log += "]";
		}
		void escape(char final) override { log += std::string("<esc ") + final + ">"; }
	};

	std::string parse(std::string_view bytes)
	{
		AnsiParser parser;
		Recorder recorder;
		for (char c : bytes)
			parser.feed(static_cast<u8>(c), recorder);
		return recorder.log;
	}
}

TEST(ansi_parser, plain_text_and_controls)
{
	CHECK_EQ(parse("ab\r\n\t\b\a"), std::string("ab<ctrl 13><ctrl 10><ctrl 9><ctrl 8><ctrl 7>"));
	CHECK_EQ(parse("a\x7F" "b"), std::string("ab"));   // DEL draws nothing
}

TEST(ansi_parser, utf8_becomes_code_points_and_bad_bytes_become_the_replacement)
{
	CHECK_EQ(parse("\xC3\xA9\xE2\x94\x80\xF0\x9F\x98\x80"), std::string("U+233U+9472U+128512"));
	CHECK_EQ(parse("\xC3" "a"), std::string("U+65533a"));        // cut short
	CHECK_EQ(parse("\x80\xFF"), std::string("U+65533U+65533"));  // no lead, no such lead
	CHECK_EQ(parse("\xED\xA0\x80"), std::string("U+65533"));     // a surrogate
}

TEST(ansi_parser, cursor_movement_sequences)
{
	CHECK_EQ(parse("\x1b[A\x1b[3B\x1b[12C\x1b[D"), std::string("[A][3B][12C][D]"));
	CHECK_EQ(parse("\x1b[5;10H\x1b[f\x1b[;7f"), std::string("[5;10H][f][0;7f]"));
}

TEST(ansi_parser, erase_sequences)
{
	CHECK_EQ(parse("\x1b[J\x1b[1J\x1b[2J\x1b[K\x1b[1K\x1b[2K"), std::string("[J][1J][2J][K][1K][2K]"));
}

TEST(ansi_parser, sgr_with_every_kind_of_parameter)
{
	CHECK_EQ(parse("\x1b[m\x1b[0;1;7;22;27m\x1b[31;42;90;101m\x1b[38;5;208;48;5;17m"),
		std::string("[m][0;1;7;22;27m][31;42;90;101m][38;5;208;48;5;17m]"));
}

TEST(ansi_parser, save_restore_region_and_the_private_cursor_sequences)
{
	CHECK_EQ(parse("\x1b[s\x1b[u\x1b[2;20r\x1b[?25l\x1b[?25h"), std::string("[s][u][2;20r][?25l][?25h]"));
	CHECK_EQ(parse("\x1b" "7\x1b" "8"), std::string("<esc 7><esc 8>"));
}

TEST(ansi_parser, what_it_does_not_know_is_consumed)
{
	CHECK_EQ(parse("a\x1b]0;a title\x07" "b\x1b]2;x\x1b\\c"), std::string("abc"));   // OSC, ended by BEL or ST
	CHECK_EQ(parse("\x1b(Bd"), std::string("<esc B>d"));                               // a charset designation
	CHECK_EQ(parse("\x1b[1 qe"), std::string("e"));                                     // an intermediate byte
	CHECK_EQ(parse("\x1b[1?2hf"), std::string("f"));                                    // a marker in the middle
}

TEST(ansi_parser, a_sequence_split_over_writes_and_one_cut_short_by_another)
{
	AnsiParser parser;
	Recorder recorder;
	for (char c : std::string_view("\x1b[3"))
		parser.feed(static_cast<u8>(c), recorder);
	CHECK(!parser.idle());
	parser.feed('A', recorder);
	CHECK(parser.idle());
	CHECK_EQ(recorder.log, std::string("[3A]"));
	CHECK_EQ(parse("\x1b[2\x1b[4B"), std::string("[4B]"));
	CHECK_EQ(parse("\x1b[2\nA"), std::string("<ctrl 10>[2A]"));   // a control inside a sequence still acts
}

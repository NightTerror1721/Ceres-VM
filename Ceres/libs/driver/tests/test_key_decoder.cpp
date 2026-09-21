#include "framework.h"
#include <ceres/driver/key_decoder.h>

#include <string>
#include <string_view>

using namespace ceres;
using namespace ceres::driver;
using namespace ceres::testing;

namespace
{
	// "t<c>" a typed character, "+<n>" a key going down, "-<n>" one going up; for a compact comparison.
	std::string describe(const std::vector<KeyAction>& actions)
	{
		std::string out;
		for (const KeyAction& action : actions)
		{
			if (!out.empty()) out += ' ';
			if (action.isText) out += "t" + std::to_string(action.value);
			else out += (action.pressed ? "+" : "-") + std::to_string(action.value);
		}
		return out;
	}

	std::string decode(std::string_view bytes, bool quiet = true)
	{
		KeyDecoder decoder;
		std::vector<KeyAction> actions;
		for (const char c : bytes)
			decoder.feed(static_cast<u8>(c), actions);
		if (quiet)
			decoder.flushPending(actions);
		return describe(actions);
	}
}

TEST(key_decoder, a_typed_letter_is_its_key_going_down_the_character_and_the_key_going_up)
{
	CHECK_EQ(decode("a"), std::string{ "+4 t97 -4" });
	CHECK_EQ(decode("Z"), std::string{ "+29 t90 -29" });       // a capital is the same key: the shift is not reported
	CHECK_EQ(decode("7"), std::string{ "+36 t55 -36" });
	CHECK_EQ(decode("0"), std::string{ "+39 t48 -39" });
	CHECK_EQ(decode(" "), std::string{ "+44 t32 -44" });
}

TEST(key_decoder, a_character_with_no_key_of_its_own_is_only_text)
{
	CHECK_EQ(decode("#"), std::string{ "t35" });
	CHECK_EQ(decode("~"), std::string{ "t126" });
}

TEST(key_decoder, enter_backspace_and_tab_are_named_keys)
{
	CHECK_EQ(decode("\r"), std::string{ "+40 -40" });
	CHECK_EQ(decode("\n"), std::string{ "+40 -40" });
	CHECK_EQ(decode("\x7f"), std::string{ "+42 -42" });
	CHECK_EQ(decode("\b"), std::string{ "+42 -42" });
	CHECK_EQ(decode("\t"), std::string{ "+43 -43" });
}

TEST(key_decoder, the_arrow_keys_in_both_of_their_spellings)
{
	CHECK_EQ(decode("\x1b[A"), std::string{ "+82 -82" });
	CHECK_EQ(decode("\x1b[B"), std::string{ "+81 -81" });
	CHECK_EQ(decode("\x1b[C"), std::string{ "+79 -79" });
	CHECK_EQ(decode("\x1b[D"), std::string{ "+80 -80" });
	CHECK_EQ(decode("\x1bOA"), std::string{ "+82 -82" });
	CHECK_EQ(decode("\x1bOD"), std::string{ "+80 -80" });
}

TEST(key_decoder, the_editing_keys)
{
	CHECK_EQ(decode("\x1b[H"), std::string{ "+74 -74" });
	CHECK_EQ(decode("\x1b[F"), std::string{ "+77 -77" });
	CHECK_EQ(decode("\x1b[1~"), std::string{ "+74 -74" });
	CHECK_EQ(decode("\x1b[4~"), std::string{ "+77 -77" });
	CHECK_EQ(decode("\x1b[2~"), std::string{ "+73 -73" });
	CHECK_EQ(decode("\x1b[3~"), std::string{ "+76 -76" });
	CHECK_EQ(decode("\x1b[5~"), std::string{ "+75 -75" });
	CHECK_EQ(decode("\x1b[6~"), std::string{ "+78 -78" });
}

TEST(key_decoder, the_function_keys)
{
	CHECK_EQ(decode("\x1bOP"), std::string{ "+58 -58" });     // F1
	CHECK_EQ(decode("\x1bOS"), std::string{ "+61 -61" });     // F4
	CHECK_EQ(decode("\x1b[15~"), std::string{ "+62 -62" });   // F5
	CHECK_EQ(decode("\x1b[17~"), std::string{ "+63 -63" });   // F6
	CHECK_EQ(decode("\x1b[21~"), std::string{ "+67 -67" });   // F10
	CHECK_EQ(decode("\x1b[23~"), std::string{ "+68 -68" });   // F11
	CHECK_EQ(decode("\x1b[24~"), std::string{ "+69 -69" });   // F12
}

TEST(key_decoder, a_modifier_parameter_does_not_hide_the_key)
{
	CHECK_EQ(decode("\x1b[1;5A"), std::string{ "+82 -82" });   // Ctrl+Up
	CHECK_EQ(decode("\x1b[3;2~"), std::string{ "+76 -76" });   // Shift+Delete
}

TEST(key_decoder, a_lone_escape_is_the_escape_key_only_once_the_input_goes_quiet)
{
	CHECK_EQ(decode("\x1b", false), std::string{});            // it could still be the start of a sequence
	CHECK_EQ(decode("\x1b"), std::string{ "+41 -41" });        // quiet: it was the key

	KeyDecoder decoder;
	std::vector<KeyAction> actions;
	decoder.feed(0x1B, actions);
	CHECK(decoder.pending());
	decoder.flushPending(actions);
	CHECK(!decoder.pending());
}

TEST(key_decoder, an_escape_followed_by_a_letter_is_the_key_and_then_the_letter)
{
	CHECK_EQ(decode("\x1b" "x"), std::string{ "+41 -41 +27 t120 -27" });
}

TEST(key_decoder, a_sequence_arriving_in_pieces_is_still_one_key)
{
	KeyDecoder decoder;
	std::vector<KeyAction> actions;
	decoder.feed(0x1B, actions);
	decoder.feed('[', actions);
	CHECK(decoder.pending());
	CHECK_EQ(describe(actions), std::string{});
	decoder.feed('A', actions);
	CHECK(!decoder.pending());
	CHECK_EQ(describe(actions), std::string{ "+82 -82" });
}

TEST(key_decoder, an_unknown_sequence_is_dropped_not_typed)
{
	CHECK_EQ(decode("\x1b[99~"), std::string{});
	CHECK_EQ(decode("\x1b[Z"), std::string{});
	CHECK_EQ(decode("\x1b[200~ab", true), std::string{ "+4 t97 -4 +5 t98 -5" });   // the bracketed-paste marker is dropped, the paste is not
}

TEST(key_decoder, utf8_characters_are_decoded_and_split_bytes_wait_for_the_rest)
{
	CHECK_EQ(decode("\xC3\xA9"), std::string{ "t233" });
	CHECK_EQ(decode("\xE2\x82\xAC"), std::string{ "t8364" });
	CHECK_EQ(decode("\xF0\x9F\x98\x80"), std::string{ "t128512" });

	KeyDecoder decoder;
	std::vector<KeyAction> actions;
	decoder.feed(0xE2, actions);
	decoder.feed(0x82, actions);
	CHECK_EQ(describe(actions), std::string{});
	decoder.feed(0xAC, actions);
	CHECK_EQ(describe(actions), std::string{ "t8364" });
}

TEST(key_decoder, a_broken_utf8_character_is_forgotten_and_the_next_byte_read_afresh)
{
	CHECK_EQ(decode("\xC3" "b"), std::string{ "+5 t98 -5" });
	CHECK_EQ(decode("\x80" "b"), std::string{ "+5 t98 -5" });   // a stray continuation byte
}

TEST(key_decoder, other_control_characters_type_nothing)
{
	CHECK_EQ(decode("\x01\x03\x04"), std::string{});
}

TEST(key_decoder, the_scancode_table)
{
	CHECK_EQ(scancodeForAscii('a'), 4u);
	CHECK_EQ(scancodeForAscii('z'), 29u);
	CHECK_EQ(scancodeForAscii('1'), 30u);
	CHECK_EQ(scancodeForAscii('9'), 38u);
	CHECK_EQ(scancodeForAscii('0'), 39u);
	CHECK_EQ(scancodeForAscii(' '), 44u);
	CHECK_EQ(scancodeForAscii('#'), 0u);
}

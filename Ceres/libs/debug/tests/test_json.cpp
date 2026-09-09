// The JSON the debug protocol is spoken in. This is the one place in the project that parses
// input from another process, so it is tested for what it *rejects* as much as what it accepts.

#include "framework.h"
#include <ceres/debug/json.h>

using namespace ceres;
using namespace ceres::testing;
using ceres::debug::json::Value;

namespace
{
	Value parseOrNull(std::string_view text)
	{
		auto parsed = Value::parse(text);
		return parsed.has_value() ? parsed.value() : Value{};
	}
}

TEST(json, primitives_survive_a_round_trip)
{
	CHECK(parseOrNull("null").isNull());
	CHECK(parseOrNull("true").asBool());
	CHECK(!parseOrNull("false").asBool(true));
	CHECK_EQ(parseOrNull("1024").asU32(), 1024u);
	CHECK_EQ(parseOrNull("-17").asNumber(), -17.0);
	CHECK_EQ(std::string(parseOrNull("\"text\"").asString()), std::string("text"));

	// Whole numbers are written back without a decimal point: everything this protocol carries is
	// an address, a line or a count, and "1024.0" would read as a mistake.
	CHECK_EQ(Value(u32{ 1024 }).serialize(), std::string("1024"));
	CHECK_EQ(Value(0.5).serialize(), std::string("0.5"));
}

TEST(json, objects_and_arrays_nest)
{
	const Value value = parseOrNull(
		R"({"a":1,"b":[1,2,{"c":"deep"}],"d":{"e":{"f":true}}})");

	CHECK(value.isObject());
	CHECK_EQ(value["a"].asU32(), 1u);
	CHECK_EQ(value["b"].asArray().size(), usize{ 3 });
	CHECK_EQ(std::string(value["b"].asArray()[2]["c"].asString()), std::string("deep"));
	CHECK(value["d"]["e"]["f"].asBool());

	// A missing key is null rather than an error, so a lookup can be chained without checking
	// every step - which is what reading a request's arguments actually looks like.
	CHECK(value["nope"].isNull());
	CHECK(value["nope"]["deeper"].isNull());
	CHECK_EQ(value["nope"].asU32(42), 42u);
	CHECK(!value.has("nope"));
	CHECK(value.has("a"));
}

TEST(json, keys_come_back_in_a_fixed_order)
{
	// Ordered rather than hashed, so the same message serialises to the same bytes every run and
	// a protocol trace can be diffed.
	const Value value = parseOrNull(R"({"zebra":1,"apple":2,"middle":3})");
	CHECK_EQ(value.serialize(), std::string(R"({"apple":2,"middle":3,"zebra":1})"));
}

TEST(json, escapes_are_read_and_written)
{
	const Value value = parseOrNull(R"("a\"b\\c\nd\teA")");
	CHECK_EQ(std::string(value.asString()), std::string("a\"b\\c\nd\te" "A"));

	// And back out again, with control characters escaped and everything else left alone.
	CHECK_EQ(Value(std::string("a\"b\\c\nd")).serialize(), std::string(R"("a\"b\\c\nd")"));
}

TEST(json, a_surrogate_pair_becomes_one_character)
{
	// U+1F41E, outside the basic plane, arrives as two \u escapes and has to be recombined - the
	// case a program printing an emoji through the terminal port would produce.
	const Value value = parseOrNull(R"("🐞")");
	const std::string text{ value.asString() };

	CHECK_EQ(text.size(), usize{ 4 });
	if (text.size() == 4)
	{
		CHECK_EQ(static_cast<u8>(text[0]), u8{ 0xF0 });
		CHECK_EQ(static_cast<u8>(text[1]), u8{ 0x9F });
		CHECK_EQ(static_cast<u8>(text[2]), u8{ 0x90 });
		CHECK_EQ(static_cast<u8>(text[3]), u8{ 0x9E });
	}
}

TEST(json, utf8_passes_through_untouched)
{
	// The Spanish tutorial's output is full of these, and escaping them would be legal JSON but
	// would make every protocol trace unreadable.
	const std::string accented = "ñandú";
	const Value value(accented);
	const std::string serialized = value.serialize();

	CHECK_EQ(serialized, std::string("\"") + accented + "\"");
	CHECK_EQ(std::string(parseOrNull(serialized).asString()), accented);
}

TEST(json, malformed_input_is_rejected_rather_than_guessed_at)
{
	const auto rejects = [](std::string_view text)
	{
		return !Value::parse(text).has_value();
	};

	CHECK(rejects(""));
	CHECK(rejects("{"));
	CHECK(rejects("[1,2"));
	CHECK(rejects("\"unterminated"));
	CHECK(rejects(R"("bad \q escape")"));
	CHECK(rejects(R"({"key" 1})"));       // Missing colon
	CHECK(rejects(R"({key: 1})"));        // Unquoted key
	CHECK(rejects("{} trailing"));        // Anything after the value
	CHECK(rejects("tru"));
	CHECK(rejects("1.2.3"));
	CHECK(rejects(R"("\u00")"));          // Truncated escape
}

TEST(json, a_deeply_nested_document_is_refused_instead_of_overflowing_the_stack)
{
	// The parser is recursive, so a hostile input could otherwise walk it off the end of the C++
	// stack. The protocol's own messages are three or four levels deep.
	std::string deep;
	for (int i = 0; i < 500; ++i)
		deep += '[';
	for (int i = 0; i < 500; ++i)
		deep += ']';

	CHECK(!Value::parse(deep).has_value());

	// A reasonable depth still works.
	std::string shallow;
	for (int i = 0; i < 20; ++i)
		shallow += '[';
	shallow += "1";
	for (int i = 0; i < 20; ++i)
		shallow += ']';
	CHECK(Value::parse(shallow).has_value());
}

TEST(json, accessors_fall_back_rather_than_throwing_on_the_wrong_type)
{
	const Value value = parseOrNull(R"({"text":"hello","number":7,"flag":true})");

	// Reading a field that is there but is not what the caller expected is a thing that happens
	// when the other side of a protocol is a different version, and it must not be fatal.
	CHECK_EQ(value["text"].asU32(99), 99u);
	CHECK_EQ(std::string(value["number"].asString("fallback")), std::string("fallback"));
	CHECK(!value["number"].asBool(false));
	CHECK(value["flag"].asArray().empty());
	CHECK(value["flag"].asObject().empty());

	// Out-of-range numbers do not wrap around into something plausible.
	CHECK_EQ(parseOrNull("-1").asU32(7), 7u);
	CHECK_EQ(parseOrNull("99999999999").asU32(7), 7u);
}

TEST(json, a_protocol_message_round_trips_unchanged)
{
	const std::string original =
		R"({"arguments":{"file":"main.casm","lines":[10,20]},"command":"setBreakpoints","seq":3,"type":"request"})";

	auto parsed = Value::parse(original);
	CHECK(parsed.has_value());
	if (!parsed.has_value()) { Registry::instance().recordFailure(parsed.error()); return; }

	CHECK_EQ(parsed->serialize(), original);
	CHECK_EQ(std::string((*parsed)["command"].asString()), std::string("setBreakpoints"));
	CHECK_EQ((*parsed)["arguments"]["lines"].asArray().size(), usize{ 2 });
	CHECK_EQ((*parsed)["arguments"]["lines"].asArray()[1].asU32(), 20u);
}

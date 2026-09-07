#include "json.h"

#include <cmath>
#include <format>

namespace ceres::debug::json
{
	namespace
	{
		const Array EmptyArray{};
		const Object EmptyObject{};
		const Value NullValue{};

		// Encodes one code point as UTF-8. \u escapes arrive as UTF-16 code units, and a character
		// outside the basic plane arrives as a surrogate pair that has to be recombined - which is
		// exactly the case a CASM program printing an emoji would produce.
		void appendUtf8(std::string& out, u32 codePoint)
		{
			if (codePoint < 0x80)
			{
				out.push_back(static_cast<char>(codePoint));
			}
			else if (codePoint < 0x800)
			{
				out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
				out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
			}
			else if (codePoint < 0x10000)
			{
				out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
				out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
			}
			else
			{
				out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
				out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
			}
		}

		class Parser
		{
		private:
			std::string_view _text;
			usize _offset = 0;
			// A hand-written recursive parser will happily follow a deeply nested input all the way
			// down the C++ stack. The protocol's own messages are three or four levels deep.
			static constexpr u32 MaxDepth = 64;

		public:
			explicit Parser(std::string_view text) noexcept : _text(text) {}

			std::expected<Value, std::string> parse()
			{
				skipWhitespace();
				auto value = parseValue(0);
				if (!value.has_value())
					return value;

				skipWhitespace();
				if (_offset != _text.size())
					return std::unexpected(std::format("Trailing characters at offset {}", _offset));

				return value;
			}

		private:
			bool atEnd() const noexcept { return _offset >= _text.size(); }
			char peek() const noexcept { return _text[_offset]; }

			void skipWhitespace() noexcept
			{
				while (!atEnd() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r'))
					++_offset;
			}

			bool consume(char expected) noexcept
			{
				if (atEnd() || peek() != expected)
					return false;
				++_offset;
				return true;
			}

			std::expected<Value, std::string> parseValue(u32 depth)
			{
				if (depth > MaxDepth)
					return std::unexpected("Nested too deeply");
				if (atEnd())
					return std::unexpected("Unexpected end of input");

				switch (peek())
				{
					case '{': return parseObject(depth);
					case '[': return parseArray(depth);
					case '"': return parseString().transform([](std::string s) { return Value(std::move(s)); });
					case 't':
						if (_text.substr(_offset).starts_with("true")) { _offset += 4; return Value(true); }
						return std::unexpected(std::format("Expected 'true' at offset {}", _offset));
					case 'f':
						if (_text.substr(_offset).starts_with("false")) { _offset += 5; return Value(false); }
						return std::unexpected(std::format("Expected 'false' at offset {}", _offset));
					case 'n':
						if (_text.substr(_offset).starts_with("null")) { _offset += 4; return Value{}; }
						return std::unexpected(std::format("Expected 'null' at offset {}", _offset));
					default:
						return parseNumber();
				}
			}

			std::expected<Value, std::string> parseObject(u32 depth)
			{
				++_offset; // '{'
				Object object;

				skipWhitespace();
				if (consume('}'))
					return Value(std::move(object));

				while (true)
				{
					skipWhitespace();
					if (atEnd() || peek() != '"')
						return std::unexpected(std::format("Expected a key at offset {}", _offset));

					auto key = parseString();
					if (!key.has_value())
						return std::unexpected(key.error());

					skipWhitespace();
					if (!consume(':'))
						return std::unexpected(std::format("Expected ':' at offset {}", _offset));

					skipWhitespace();
					auto value = parseValue(depth + 1);
					if (!value.has_value())
						return value;

					object.insert_or_assign(std::move(key.value()), std::move(value.value()));

					skipWhitespace();
					if (consume(','))
						continue;
					if (consume('}'))
						return Value(std::move(object));

					return std::unexpected(std::format("Expected ',' or '}}' at offset {}", _offset));
				}
			}

			std::expected<Value, std::string> parseArray(u32 depth)
			{
				++_offset; // '['
				Array array;

				skipWhitespace();
				if (consume(']'))
					return Value(std::move(array));

				while (true)
				{
					skipWhitespace();
					auto value = parseValue(depth + 1);
					if (!value.has_value())
						return value;

					array.push_back(std::move(value.value()));

					skipWhitespace();
					if (consume(','))
						continue;
					if (consume(']'))
						return Value(std::move(array));

					return std::unexpected(std::format("Expected ',' or ']' at offset {}", _offset));
				}
			}

			std::expected<std::string, std::string> parseString()
			{
				++_offset; // '"'
				std::string out;

				while (true)
				{
					if (atEnd())
						return std::unexpected("Unterminated string");

					const char c = _text[_offset++];
					if (c == '"')
						return out;

					if (c != '\\')
					{
						out.push_back(c);
						continue;
					}

					if (atEnd())
						return std::unexpected("Unterminated escape");

					switch (const char escape = _text[_offset++])
					{
						case '"':  out.push_back('"'); break;
						case '\\': out.push_back('\\'); break;
						case '/':  out.push_back('/'); break;
						case 'b':  out.push_back('\b'); break;
						case 'f':  out.push_back('\f'); break;
						case 'n':  out.push_back('\n'); break;
						case 'r':  out.push_back('\r'); break;
						case 't':  out.push_back('\t'); break;
						case 'u':
						{
							auto first = parseHex4();
							if (!first.has_value())
								return std::unexpected(first.error());

							u32 codePoint = first.value();
							// A high surrogate is only half a character; the low half follows as a
							// second \u escape.
							if (codePoint >= 0xD800 && codePoint <= 0xDBFF &&
								_text.substr(_offset).starts_with("\\u"))
							{
								const usize mark = _offset;
								_offset += 2;
								auto second = parseHex4();
								if (second.has_value() && second.value() >= 0xDC00 && second.value() <= 0xDFFF)
								{
									codePoint = 0x10000 +
										((codePoint - 0xD800) << 10) +
										(second.value() - 0xDC00);
								}
								else
								{
									_offset = mark; // Not a pair after all; leave the lone surrogate.
								}
							}
							appendUtf8(out, codePoint);
							break;
						}
						default:
							return std::unexpected(std::format("Unknown escape '\\{}'", escape));
					}
				}
			}

			std::expected<u32, std::string> parseHex4()
			{
				if (_offset + 4 > _text.size())
					return std::unexpected("Truncated \\u escape");

				u32 value = 0;
				for (usize i = 0; i < 4; ++i)
				{
					const char c = _text[_offset + i];
					value <<= 4;
					if (c >= '0' && c <= '9')      value |= static_cast<u32>(c - '0');
					else if (c >= 'a' && c <= 'f') value |= static_cast<u32>(c - 'a' + 10);
					else if (c >= 'A' && c <= 'F') value |= static_cast<u32>(c - 'A' + 10);
					else return std::unexpected("Bad hex digit in \\u escape");
				}
				_offset += 4;
				return value;
			}

			std::expected<Value, std::string> parseNumber()
			{
				const usize start = _offset;
				if (!atEnd() && peek() == '-')
					++_offset;

				while (!atEnd() && ((peek() >= '0' && peek() <= '9') || peek() == '.' ||
					peek() == 'e' || peek() == 'E' || peek() == '+' || peek() == '-'))
				{
					++_offset;
				}

				if (start == _offset)
					return std::unexpected(std::format("Expected a value at offset {}", start));

				// from_chars for double is not available everywhere this builds, and the numbers
				// crossing this protocol are addresses and line numbers rather than anything that
				// needs perfect round-tripping.
				const std::string text{ _text.substr(start, _offset - start) };
				try
				{
					usize consumed = 0;
					const double value = std::stod(text, &consumed);
					if (consumed != text.size())
						return std::unexpected(std::format("Malformed number '{}'", text));
					return Value(value);
				}
				catch (const std::exception&)
				{
					return std::unexpected(std::format("Malformed number '{}'", text));
				}
			}
		};
	}

	// --- Accessors ------------------------------------------------------------------------------

	bool Value::asBool(bool fallback) const noexcept
	{
		if (const bool* value = std::get_if<bool>(&_value))
			return *value;
		return fallback;
	}

	double Value::asNumber(double fallback) const noexcept
	{
		if (const double* value = std::get_if<double>(&_value))
			return *value;
		return fallback;
	}

	u32 Value::asU32(u32 fallback) const noexcept
	{
		if (const double* value = std::get_if<double>(&_value))
		{
			if (*value < 0.0 || *value > 4294967295.0 || std::isnan(*value))
				return fallback;
			return static_cast<u32>(*value);
		}
		return fallback;
	}

	u64 Value::asU64(u64 fallback) const noexcept
	{
		if (const double* value = std::get_if<double>(&_value))
		{
			if (*value < 0.0 || std::isnan(*value))
				return fallback;
			return static_cast<u64>(*value);
		}
		return fallback;
	}

	std::string_view Value::asString(std::string_view fallback) const noexcept
	{
		if (const std::string* value = std::get_if<std::string>(&_value))
			return *value;
		return fallback;
	}

	const Array& Value::asArray() const noexcept
	{
		if (const auto* value = std::get_if<std::shared_ptr<Array>>(&_value))
			return **value;
		return EmptyArray;
	}

	const Object& Value::asObject() const noexcept
	{
		if (const auto* value = std::get_if<std::shared_ptr<Object>>(&_value))
			return **value;
		return EmptyObject;
	}

	const Value& Value::operator[](std::string_view key) const noexcept
	{
		const Object& object = asObject();
		const auto it = object.find(key);
		return it == object.end() ? NullValue : it->second;
	}

	bool Value::has(std::string_view key) const noexcept
	{
		const Object& object = asObject();
		return object.find(key) != object.end();
	}

	// --- Writing --------------------------------------------------------------------------------

	std::string escape(std::string_view text)
	{
		std::string out;
		out.reserve(text.size() + 2);
		for (char c : text)
		{
			switch (c)
			{
				case '"':  out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				case '\b': out += "\\b"; break;
				case '\f': out += "\\f"; break;
				default:
					// Only control characters have to be escaped; anything else, including a UTF-8
					// continuation byte, is passed through so multi-byte text survives intact.
					if (static_cast<unsigned char>(c) < 0x20)
						out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
					else
						out += c;
			}
		}
		return out;
	}

	std::string Value::serialize() const
	{
		if (isNull())
			return "null";

		if (const bool* value = std::get_if<bool>(&_value))
			return *value ? "true" : "false";

		if (const double* value = std::get_if<double>(&_value))
		{
			// Whole numbers are written without a decimal point: every number this protocol
			// carries is an address, a line or a count, and "1024.0" would read as a mistake.
			if (std::isfinite(*value) && *value == std::floor(*value) &&
				std::abs(*value) < 9007199254740992.0)
			{
				return std::format("{}", static_cast<i64>(*value));
			}
			if (!std::isfinite(*value))
				return "null"; // JSON has no infinity or NaN.
			return std::format("{}", *value);
		}

		if (const std::string* value = std::get_if<std::string>(&_value))
			return std::format("\"{}\"", escape(*value));

		if (isArray())
		{
			std::string out = "[";
			bool first = true;
			for (const Value& element : asArray())
			{
				if (!first)
					out += ',';
				first = false;
				out += element.serialize();
			}
			out += ']';
			return out;
		}

		std::string out = "{";
		bool first = true;
		for (const auto& [key, value] : asObject())
		{
			if (!first)
				out += ',';
			first = false;
			out += std::format("\"{}\":{}", escape(key), value.serialize());
		}
		out += '}';
		return out;
	}

	std::expected<Value, std::string> Value::parse(std::string_view text)
	{
		return Parser{ text }.parse();
	}
}

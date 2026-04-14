#include "lexer.h"
#include <charconv>
#include <limits>

namespace ceres::casm
{
	Token Lexer::nextToken()
	{
		skipWhitespaceAndComments();

		if (!_source)
			return Token::makeEndOfFile(_source.line(), _source.column());

		usize startPosition = _source.position();
		u32 startColumn = _source.column();
        char currentChar = _source.next();

		if (Identifier::isAsciiAlpha(currentChar) || currentChar == '_')
			return scanIdentifierOrKeyword(startPosition, startColumn);

		if (Identifier::isAsciiDigit(currentChar))
			return scanNumberLiteral(startPosition, startColumn);

		if (currentChar == '0' && (*_source == 'x' || *_source == 'X' || *_source == 'b' || *_source == 'B'))
			return scanNumberLiteral(startPosition, startColumn);

        if (currentChar == '-' || currentChar == '+')
		{
			if (Identifier::isAsciiDigit(*_source))
				return scanNumberLiteral(startPosition, startColumn);

			if (*_source == '.' && Identifier::isAsciiDigit(_source[1]))
				return scanNumberLiteral(startPosition, startColumn);

			if (*_source == '0' && (_source[1] == 'x' || _source[1] == 'X' || _source[1] == 'b' || _source[1] == 'B'))
				return scanNumberLiteral(startPosition, startColumn);
		}

		if (currentChar == '.' && Identifier::isAsciiDigit(*_source))
			return scanNumberLiteral(startPosition, startColumn);

		if (currentChar == '"')
			return scanStringLiteral(startPosition, startColumn);

		if (currentChar == '\'')
			return scanCharLiteral(startPosition, startColumn);

		switch (currentChar)
		{
			case '@': return Token::makeAt(_source.line(), startColumn);
			case ':': return Token::makeColon(_source.line(), startColumn);
			case '.': return Token::makeDot(_source.line(), startColumn);
			case ',': return Token::makeComma(_source.line(), startColumn);
			case '=': return Token::makeEquals(_source.line(), startColumn);
			case '+': return Token::makePlus(_source.line(), startColumn);
			case '-': return Token::makeMinus(_source.line(), startColumn);
			case '*': return Token::makeAsterisk(_source.line(), startColumn);
			case '/': return Token::makeSlash(_source.line(), startColumn);
			case '[': return Token::makeBracketOpen(_source.line(), startColumn);
			case ']': return Token::makeBracketClose(_source.line(), startColumn);
			case '\n': return Token::makeEndOfLine(_source.line(), startColumn);
		}

		return Token::makeInvalid(_source.line(), startColumn);
	}

	void Lexer::skipWhitespaceAndComments() noexcept
	{
		while (!_source)
		{
			switch (char currentChar = *_source; currentChar)
			{
				case ' ':
				case '\t':
				case '\r':
					++_source;
					break;

				case '/':
					if (_source[1] == '/')
					{
						++_source; // Consume the first '/'
						++_source; // Consume the second '/'
						while (*_source != '\n' && _source)
							++_source;
					}
					else if (_source[1] == '*')
					{
						++_source; // Consume the first '/'
						++_source; // Consume the '*'

						while (!_source || !(*_source == '*' && _source[1] == '/'))
							++_source;

						if (!_source)
						{
							++_source; // Consume the '*'
							++_source; // Consume the '/'
						}
					}
					else
					{
						return; // Not a comment, exit the loop
					}
					break;

				default:
					return; // Not whitespace or a comment, exit the loop
			}
		}
	}

	Token Lexer::scanIdentifierOrKeyword(usize startPosition, u32 startColumn) noexcept
	{
		usize count = _source.skipUntil(+[](char ch) { return !Identifier::isAsciiAlnum(ch) && ch != '_'; });
		std::string_view text = _source.peekSourceUntilCurrentPosition(count);

		const auto keywordType = checkKeyword(text);
		if (keywordType.has_value())
			return Token::makeKeyword(text, *keywordType, _source.line(), startColumn);

		const auto dataType = checkDataType(text);
		if (dataType.has_value())
			return Token::makeDataType(text, *dataType, _source.line(), startColumn);

		return Token::makeIdentifier(text, _source.line(), startColumn);
	}

	Token Lexer::scanNumberLiteral(usize startPosition, u32 startColumn) noexcept
	{
		const std::string_view src = _source.source();
		usize idx = startPosition;

		bool isNegative = false;
		if (idx < src.size())
		{
			if (src[idx] == '-')
			{
				isNegative = true;
				idx++;
			}
			else if (src[idx] == '+')
			{
				// explicit plus sign: consume but not mark negative
				idx++;
			}
		}

		int base = 10;
		usize digitsStart = idx;
		// Hex and binary prefixes -> integer only
		if (idx + 1 < src.size() && src[idx] == '0' && (src[idx + 1] == 'x' || src[idx + 1] == 'X'))
		{
			base = 16;
			idx += 2;
			digitsStart = idx;
		}
		else if (idx + 1 < src.size() && src[idx] == '0' && (src[idx + 1] == 'b' || src[idx + 1] == 'B'))
		{
			base = 2;
			idx += 2;
			digitsStart = idx;
		}

		bool isFloat = false;
		// For base 10 support optional fractional part and exponent
		if (base == 10)
		{
			// Integer part (may be absent for ".5")
			bool hasDigitsBeforeDot = false;
			while (idx < src.size() && Identifier::isAsciiDigit(src[idx]))
			{
				hasDigitsBeforeDot = true;
				++idx;
			}

			// Fractional part
			if (idx < src.size() && src[idx] == '.')
			{
				isFloat = true;
				++idx; // consume '.'
				bool hasDigitsAfterDot = false;
				while (idx < src.size() && Identifier::isAsciiDigit(src[idx]))
				{
					hasDigitsAfterDot = true;
					++idx;
				}

				if (!hasDigitsBeforeDot && !hasDigitsAfterDot)
					return Token::makeInvalid(_source.line(), startColumn);
			}

			// Exponent part
			if (idx < src.size() && (src[idx] == 'e' || src[idx] == 'E'))
			{
				isFloat = true;
				++idx; // consume 'e' or 'E'
				if (idx < src.size() && (src[idx] == '+' || src[idx] == '-'))
					++idx;

				if (idx >= src.size() || !Identifier::isAsciiDigit(src[idx]))
					return Token::makeInvalid(_source.line(), startColumn);

				while (idx < src.size() && Identifier::isAsciiDigit(src[idx]))
					++idx;
			}
		}
		else
		{
			// Non-decimal integer parsing
			while (idx < src.size() && isValidDigit(static_cast<unsigned char>(src[idx]), base))
				++idx;
		}

		// Ensure we consumed at least one digit for integer/binary/hex cases
		if (!isFloat && idx == digitsStart)
			return Token::makeInvalid(_source.line(), startColumn);

		usize endIndex = idx;
		_source.next(static_cast<usize>(endIndex - _source.position())); // Move the source position to the end of the literal

		std::string_view text = src.substr(startPosition, endIndex - startPosition);

		if (isFloat)
		{
			// Parse as floating point (base 10 only) without allocations using std::from_chars.
			float value = 0.0f;
			auto res = std::from_chars(text.data(), text.data() + text.size(), value);
			if (res.ec != std::errc() || res.ptr != text.data() + text.size())
				return Token::makeInvalid(_source.line(), startColumn);

			return Token::makeLiteralFloat(text, value, _source.line(), startColumn);
		}
		else
		{
			// Integer path (existing behavior)
			std::string_view numberPart = text;

			if (!numberPart.empty() && numberPart[0] == '-')
				numberPart.remove_prefix(1);
			if (numberPart.size() > 1 && numberPart[0] == '0' && (numberPart[1] == 'x' || numberPart[1] == 'X' || numberPart[1] == 'b' || numberPart[1] == 'B'))
				numberPart.remove_prefix(2);

			u64 tmp = 0;
			auto result = std::from_chars(numberPart.data(), numberPart.data() + numberPart.size(), tmp, base);
			if (result.ec != std::errc() || result.ptr != numberPart.data() + numberPart.size())
				return Token::makeInvalid(_source.line(), startColumn);

			constexpr u64 NEG_LIMIT = static_cast<u64>(-(static_cast<i64>(std::numeric_limits<i32>::min())));
			constexpr u64 POS_LIMIT = static_cast<u64>(std::numeric_limits<u32>::max());

			if (isNegative)
			{
				if (tmp > NEG_LIMIT)
					return Token::makeInvalid(_source.line(), startColumn);
				u32 value = static_cast<u32>(-static_cast<i64>(tmp));
				return Token::makeLiteralInteger(text, value, _source.line(), startColumn);
			}
			else
			{
				if (tmp > POS_LIMIT)
					return Token::makeInvalid(_source.line(), startColumn);
				u32 value = static_cast<u32>(tmp);
				return Token::makeLiteralInteger(text, value, _source.line(), startColumn);
			}
		}
	}

   Token Lexer::scanStringLiteral(usize startPosition, u32 startColumn) noexcept
	{
		std::string stringContentBuilder{ 16 }; // Start with a small capacity to avoid unnecessary allocations for short strings

		while (_source && *_source != '"')
		{
			char currentChar = *_source;
			// Unescaped newlines are not allowed inside string literals
			if (currentChar == '\n' || currentChar == '\r')
				return Token::makeInvalid(_source.line(), startColumn);

			if (currentChar == '\\')
			{
				++_source; // consume backslash
				if (!_source)
					return Token::makeInvalid(_source.line(), startColumn);

				char c = *_source;
				switch (c)
				{
					case 'n': stringContentBuilder.push_back('\n'); ++_source; break;
					case 't': stringContentBuilder.push_back('\t'); ++_source; break;
					case 'r': stringContentBuilder.push_back('\r'); ++_source; break;
					case '\\': stringContentBuilder.push_back('\\'); ++_source; break;
					case '"': stringContentBuilder.push_back('"'); ++_source; break;
					case '\'': stringContentBuilder.push_back('\''); ++_source; break;
					case '0': stringContentBuilder.push_back('\0'); ++_source; break;
					case 'x':
					{
						// Expect exactly two hex digits after \x
						// Ensure there are two characters available
						if (!_source || !std::isxdigit(static_cast<unsigned char>(_source[1])) || !std::isxdigit(static_cast<unsigned char>(_source[2])))
							return Token::makeInvalid(_source.line(), startColumn);

						// consume 'x'
						++_source;
						unsigned char hi = static_cast<unsigned char>(*_source);
						++_source;
						unsigned char lo = static_cast<unsigned char>(*_source);
						int hiVal = hexValue(hi);
						int loVal = hexValue(lo);
						if (hiVal < 0 || loVal < 0)
							return Token::makeInvalid(_source.line(), startColumn);

						unsigned char value = static_cast<unsigned char>((hiVal << 4) | loVal);
						stringContentBuilder.push_back(static_cast<char>(value));
						++_source; // move past second hex digit
						break;
					}
					default:
						return Token::makeInvalid(_source.line(), startColumn);
				}
			}
			else
			{
				// Regular character
				stringContentBuilder.push_back(currentChar);
				++_source;
			}
		}

		if (!_source)
			return Token::makeInvalid(_source.line(), startColumn);

		++_source; // consume closing '"'

		std::string_view fullLexeme = _source.getSourceSubpart(startPosition, _source.position() - startPosition);

		return Token::makeLiteralString(fullLexeme, std::move(stringContentBuilder), _source.line(), startColumn);
	}

	Token Lexer::scanCharLiteral(usize startPosition, u32 startColumn) noexcept
	{
		usize charContentStart = _source.position();

		if (!_source)
			return Token::makeInvalid(_source.line(), startColumn);

        char charValue = *_source;
		if (charValue == '\n' || charValue == '\'')
			return Token::makeInvalid(_source.line(), startColumn);

		if (charValue == '\\')
		{
			++_source; // Consume the backslash
			if (!_source)
				return Token::makeInvalid(_source.line(), startColumn);
			switch (char c = *_source)
			{
				case 'n': charValue = '\n'; break;
				case 't': charValue = '\t'; break;
				case 'r': charValue = '\r'; break;
				case '\\': charValue = '\\'; break;
				case '"': charValue = '"'; break;
				case '\'': charValue = '\''; break;
				case '0': charValue = '\0'; break;
				case 'x':
				{
					// Expect exactly two hex digits after \x
					// Ensure there are two characters available
					if (!_source || !std::isxdigit(static_cast<unsigned char>(_source[1])) || !std::isxdigit(static_cast<unsigned char>(_source[2])))
						return Token::makeInvalid(_source.line(), startColumn);

					// consume 'x'
					++_source;
					unsigned char hi = static_cast<unsigned char>(*_source);
					++_source;
					unsigned char lo = static_cast<unsigned char>(*_source);
					int hiVal = hexValue(hi);
					int loVal = hexValue(lo);
					if (hiVal < 0 || loVal < 0)
						return Token::makeInvalid(_source.line(), startColumn);

					charValue = static_cast<char>(static_cast<unsigned char>((hiVal << 4) | loVal));
					++_source; // move past second hex digit
					break;
				}
				default:
					return Token::makeInvalid(_source.line(), startColumn);
			}
		}
		++_source;

		if (!_source || *_source != '\'')
			return Token::makeInvalid(_source.line(), startColumn);
		++_source;

		std::string_view fullLexeme = _source.getSourceSubpart(startPosition, _source.position() - startPosition);

		return Token::makeLiteralChar(fullLexeme, static_cast<u8>(charValue), _source.line(), startColumn);
	}

	std::optional<KeywordType> Lexer::checkKeyword(std::string_view identifier) noexcept
	{
		if (identifier == "let") return KeywordType::Let;
		if (identifier == "const") return KeywordType::Constant;
		if (identifier == "global") return KeywordType::Global;

		return std::nullopt;
	}

	std::optional<DataType> Lexer::checkDataType(std::string_view identifier) noexcept
	{
		if (identifier == "u8") return DataType::U8;
		if (identifier == "u16") return DataType::U16;
		if (identifier == "u32") return DataType::U32;
		if (identifier == "string") return DataType::String;

		return std::nullopt;
	}
}
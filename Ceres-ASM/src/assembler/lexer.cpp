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
		// Captured before next() consumes the character: for a newline that call already advanced
		// the counter, so every end-of-line token used to be reported on the following line.
		const u32 startLine = _source.line();
        char currentChar = _source.next();

		if (Identifier::isAsciiAlpha(currentChar) || currentChar == '_')
			return scanIdentifierOrKeyword(startPosition, startLine, startColumn);

		if (Identifier::isAsciiDigit(currentChar))
			return scanNumberLiteral(startPosition, startLine, startColumn);

		if (currentChar == '0' && (*_source == 'x' || *_source == 'X' || *_source == 'b' || *_source == 'B'))
			return scanNumberLiteral(startPosition, startLine, startColumn);

        if (currentChar == '-' || currentChar == '+')
		{
			if (Identifier::isAsciiDigit(*_source))
				return scanNumberLiteral(startPosition, startLine, startColumn);

			if (*_source == '.' && Identifier::isAsciiDigit(_source[1]))
				return scanNumberLiteral(startPosition, startLine, startColumn);

			if (*_source == '0' && (_source[1] == 'x' || _source[1] == 'X' || _source[1] == 'b' || _source[1] == 'B'))
				return scanNumberLiteral(startPosition, startLine, startColumn);
		}

		if (currentChar == '.' && Identifier::isAsciiDigit(*_source))
			return scanNumberLiteral(startPosition, startLine, startColumn);

		if (currentChar == '"')
			return scanStringLiteral(startPosition, startLine, startColumn);

		if (currentChar == '\'')
			return scanCharLiteral(startPosition, startLine, startColumn);

		if (currentChar == '$')
		{
			if (Identifier::isAsciiAlpha(*_source) || *_source == '_')
				return scanSpecialIdentifier(startPosition, startLine, startColumn, SpecialIdentifierType::DollarIdentifier);
			else
				return Token::makeInvalid(startLine, startColumn);
		}

		if (currentChar == '%')
		{
			if (*_source == '%')
			{
				_source.next(); // Consume the second '%'
				if (Identifier::isAsciiAlpha(*_source) || *_source == '_')
					return scanSpecialIdentifier(startPosition, startLine, startColumn, SpecialIdentifierType::DoublePercentIdentifier);
				else
					return Token::makeInvalid(startLine, startColumn);
			}
			else
			{
				return Token::makeInvalid(startLine, startColumn);
			}
		}

		switch (currentChar)
		{
			case '@': return Token::makeAt(startLine, startColumn);
			case ':': return Token::makeColon(startLine, startColumn);
			case '.': return Token::makeDot(startLine, startColumn);
			case ',': return Token::makeComma(startLine, startColumn);
			case '=': return Token::makeEquals(startLine, startColumn);
			case '+': return Token::makePlus(startLine, startColumn);
			case '-': return Token::makeMinus(startLine, startColumn);
			case '*': return Token::makeAsterisk(startLine, startColumn);
			case '/': return Token::makeSlash(startLine, startColumn);
			case '[': return Token::makeBracketOpen(startLine, startColumn);
			case ']': return Token::makeBracketClose(startLine, startColumn);
			case '(': return Token::makeParenOpen(startLine, startColumn);
			case ')': return Token::makeParenClose(startLine, startColumn);
			case '\n': return Token::makeEndOfLine(startLine, startColumn);
		}

		return Token::makeInvalid(startLine, startColumn);
	}

	void Lexer::skipWhitespaceAndComments() noexcept
	{
		while (_source)
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

						while (_source && !(*_source == '*' && _source[1] == '/'))
							++_source;

						if (_source)
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

	Token Lexer::scanIdentifierOrKeyword(usize startPosition, u32 startLine, u32 startColumn) noexcept
	{
		usize count = _source.skipUntil(+[](char ch) { return !Identifier::isAsciiAlnum(ch) && ch != '_'; }) + 1;
		std::string_view text = _source.peekSourceUntilCurrentPosition(count);

		const auto keywordType = checkKeyword(text);
		if (keywordType.has_value())
			return Token::makeKeyword(text, *keywordType, startLine, startColumn);

		const auto dataType = DataType::fromString(text);
		if (dataType.has_value())
			return Token::makeDataType(text, *dataType, startLine, startColumn);

		return Token::makeIdentifier(text, _stringPool.makeIdentifier(text), startLine, startColumn);
	}

	Token Lexer::scanSpecialIdentifier(usize startPosition, u32 startLine, u32 startColumn, SpecialIdentifierType type) noexcept
	{
		usize count = _source.skipUntil(+[](char ch) { return !Identifier::isAsciiAlnum(ch) && ch != '_'; }) + 1;
		std::string_view text = _source.peekSourceUntilCurrentPosition(count);

		const auto keywordType = checkKeyword(text);
		if (keywordType.has_value())
			return Token::makeInvalid(startLine, startColumn); // Identifiers cannot be keywords, so return an invalid token.

		const auto dataType = DataType::fromString(text);
		if (dataType.has_value())
			return Token::makeInvalid(startLine, startColumn); // Identifiers cannot be data types, so return an invalid token.

		std::string_view lexeme = _source.getSourceSubpart(startPosition, _source.position() - startPosition);

		switch (type)
		{
			case SpecialIdentifierType::DollarIdentifier:
				return Token::makeDollarIdentifier(lexeme, _stringPool.makeIdentifier(text), startLine, startColumn);

			case SpecialIdentifierType::DoublePercentIdentifier:
				return Token::makeDoublePercentIdentifier(lexeme, _stringPool.makeIdentifier(text), startLine, startColumn);

			default:
				return Token::makeInvalid(startLine, startColumn); // Fallback case, should not be reached.
		}
	}

	Token Lexer::scanNumberLiteral(usize startPosition, u32 startLine, u32 startColumn) noexcept
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
					return Token::makeInvalid(startLine, startColumn);
			}

			// Exponent part
			if (idx < src.size() && (src[idx] == 'e' || src[idx] == 'E'))
			{
				isFloat = true;
				++idx; // consume 'e' or 'E'
				if (idx < src.size() && (src[idx] == '+' || src[idx] == '-'))
					++idx;

				if (idx >= src.size() || !Identifier::isAsciiDigit(src[idx]))
					return Token::makeInvalid(startLine, startColumn);

				while (idx < src.size() && isAsciiDigit(src[idx]))
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
			return Token::makeInvalid(startLine, startColumn);

		usize endIndex = idx;
		_source.next(static_cast<usize>(endIndex - _source.position())); // Move the source position to the end of the literal

		std::string_view text = src.substr(startPosition, endIndex - startPosition);

		if (isFloat)
		{
			// Parse as floating point (base 10 only) without allocations using std::from_chars.
			float value = 0.0f;
			auto res = std::from_chars(text.data(), text.data() + text.size(), value);
			if (res.ec != std::errc() || res.ptr != text.data() + text.size())
				return Token::makeInvalid(startLine, startColumn);

			return Token::makeLiteralFloat(text, value, startLine, startColumn);
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
				return Token::makeInvalid(startLine, startColumn);

			constexpr u64 NEG_LIMIT = static_cast<u64>(-(static_cast<i64>(std::numeric_limits<i32>::min())));
			constexpr u64 POS_LIMIT = static_cast<u64>(std::numeric_limits<u32>::max());

			if (isNegative)
			{
				if (tmp > NEG_LIMIT)
					return Token::makeInvalid(startLine, startColumn);
				u32 value = static_cast<u32>(-static_cast<i64>(tmp));
				return Token::makeLiteralInteger(text, value, startLine, startColumn);
			}
			else
			{
				if (tmp > POS_LIMIT)
					return Token::makeInvalid(startLine, startColumn);
				u32 value = static_cast<u32>(tmp);
				return Token::makeLiteralInteger(text, value, startLine, startColumn);
			}
		}
	}

   Token Lexer::scanStringLiteral(usize startPosition, u32 startLine, u32 startColumn) noexcept
	{
		std::string stringContentBuilder;
		stringContentBuilder.reserve(16); // Start with a small capacity to avoid unnecessary allocations for short strings

		while (_source && *_source != '"')
		{
			char currentChar = *_source;
			// Unescaped newlines are not allowed inside string literals
			if (currentChar == '\n' || currentChar == '\r')
				return Token::makeInvalid(startLine, startColumn);

			if (currentChar == '\\')
			{
				++_source; // consume backslash
				if (!_source)
					return Token::makeInvalid(startLine, startColumn);

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
							return Token::makeInvalid(startLine, startColumn);

						// consume 'x'
						++_source;
						unsigned char hi = static_cast<unsigned char>(*_source);
						++_source;
						unsigned char lo = static_cast<unsigned char>(*_source);
						int hiVal = hexValue(hi);
						int loVal = hexValue(lo);
						if (hiVal < 0 || loVal < 0)
							return Token::makeInvalid(startLine, startColumn);

						unsigned char value = static_cast<unsigned char>((hiVal << 4) | loVal);
						stringContentBuilder.push_back(static_cast<char>(value));
						++_source; // move past second hex digit
						break;
					}
					default:
						return Token::makeInvalid(startLine, startColumn);
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
			return Token::makeInvalid(startLine, startColumn);

		++_source; // consume closing '"'

		std::string_view fullLexeme = _source.getSourceSubpart(startPosition, _source.position() - startPosition);

		return Token::makeLiteralString(fullLexeme, _stringPool.makeLiteralString(std::move(stringContentBuilder)), startLine, startColumn);
	}

	Token Lexer::scanCharLiteral(usize startPosition, u32 startLine, u32 startColumn) noexcept
	{
		usize charContentStart = _source.position();

		if (!_source)
			return Token::makeInvalid(startLine, startColumn);

        char charValue = *_source;
		if (charValue == '\n' || charValue == '\'')
			return Token::makeInvalid(startLine, startColumn);

		if (charValue == '\\')
		{
			++_source; // Consume the backslash
			if (!_source)
				return Token::makeInvalid(startLine, startColumn);
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
						return Token::makeInvalid(startLine, startColumn);

					// consume 'x'
					++_source;
					unsigned char hi = static_cast<unsigned char>(*_source);
					++_source;
					unsigned char lo = static_cast<unsigned char>(*_source);
					int hiVal = hexValue(hi);
					int loVal = hexValue(lo);
					if (hiVal < 0 || loVal < 0)
						return Token::makeInvalid(startLine, startColumn);

					charValue = static_cast<char>(static_cast<unsigned char>((hiVal << 4) | loVal));
					++_source; // move past second hex digit
					break;
				}
				default:
					return Token::makeInvalid(startLine, startColumn);
			}
		}
		++_source;

		if (!_source || *_source != '\'')
			return Token::makeInvalid(startLine, startColumn);
		++_source;

		std::string_view fullLexeme = _source.getSourceSubpart(startPosition, _source.position() - startPosition);

		return Token::makeLiteralChar(fullLexeme, static_cast<u8>(charValue), startLine, startColumn);
	}

	std::optional<KeywordType> Lexer::checkKeyword(std::string_view identifier) noexcept
	{
		if (identifier == "let") return KeywordType::Let;
		if (identifier == "const") return KeywordType::Constant;
		if (identifier == "global") return KeywordType::Global;
		if (identifier == "import") return KeywordType::Import;
		if (identifier == "macro") return KeywordType::Macro;
		if (identifier == "endmacro") return KeywordType::EndMacro;

		return std::nullopt;
	}
}
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

		if (std::isalpha(static_cast<unsigned char>(currentChar)) || currentChar == '_')
			return scanIdentifierOrKeyword(startPosition, startColumn);

		if (std::isdigit(static_cast<unsigned char>(currentChar)))
			return scanIntegerLiteral(startPosition, startColumn);

		if (currentChar == '0' && (*_source == 'x' || *_source == 'X' || *_source == 'b' || *_source == 'B'))
			return scanIntegerLiteral(startPosition, startColumn);

		if (currentChar == '-')
		{
			if (std::isdigit(*_source))
				return scanIntegerLiteral(startPosition, startColumn);

			if (*_source == '0' && (_source[1] == 'x' || _source[1] == 'X' || _source[1] == 'b' || _source[1] == 'B'))
				return scanIntegerLiteral(startPosition, startColumn);
		}

		if (currentChar == '"')
			return scanStringLiteral(startPosition, startColumn);

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
				case '\n':
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
		usize count = _source.skipUntil(+[](char ch) { return !std::isalnum(static_cast<unsigned char>(ch)) && ch != '_'; });
		std::string_view text = _source.peekSourceUntilCurrentPosition(count);

		const auto keywordType = checkKeyword(text);
		if (keywordType.has_value())
			return Token::makeKeyword(text, *keywordType, _source.line(), startColumn);

		const auto dataType = checkDataType(text);
		if (dataType.has_value())
			return Token::makeDataType(text, *dataType, _source.line(), startColumn);

		return Token::makeIdentifier(text, _source.line(), startColumn);
	}

	Token Lexer::scanIntegerLiteral(usize startPosition, u32 startColumn) noexcept
	{
		const std::string_view src = _source.source();
		usize idx = startPosition;

		bool isNegative = false;
		if (idx < src.size() && src[idx] == '-')
		{
			isNegative = true;
			idx++;
		}

		int base = 10;
		usize digitsStart = idx;
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

		while (idx < src.size() && isValidDigit(static_cast<unsigned char>(src[idx]), base))
			++idx;

		if (idx == digitsStart)
			return Token::makeInvalid(_source.line(), startColumn);

		usize endIndex = idx;

		_source.next(endIndex - _source.position()); // Move the source position to the end of the integer literal

		std::string_view text = src.substr(startPosition, endIndex - startPosition);
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

	Token Lexer::scanStringLiteral(usize startPosition, u32 startColumn) noexcept
	{
		usize stringContentStart = _source.position();
		std::string stringContentBuilder{ 16 }; // Start with a small capacity to avoid unnecessary allocations for short strings

		while (_source && *_source != '"')
		{
			char currentChar = *_source;
			if (currentChar == '\n')
			{
				stringContentBuilder.push_back('\n');
			}
			else if (currentChar == '\\')
			{
				++_source; // Consume the backslash
				if (!_source)
					return Token::makeInvalid(_source.line(), startColumn);

				switch (char c = *_source)
				{
					case 'n': stringContentBuilder.push_back('\n'); break;
					case 't': stringContentBuilder.push_back('\t'); break;
					case 'r': stringContentBuilder.push_back('\r'); break;
					case '\\': stringContentBuilder.push_back('\\'); break;
					case '"': stringContentBuilder.push_back('"'); break;
					case '\'': stringContentBuilder.push_back('\''); break;
					case '0': stringContentBuilder.push_back('\0'); break;
					default:
						return Token::makeInvalid(_source.line(), startColumn);
				}
			}
			else
			{
				stringContentBuilder.push_back(currentChar);
			}
			++_source;
		}

		if (!_source)
			return Token::makeInvalid(_source.line(), startColumn);

		++_source;

		std::string_view fullLexeme = _source.getSourceSubpart(startPosition, _source.position() - startPosition);

		return Token::makeLiteralString(fullLexeme, std::move(stringContentBuilder), _source.line(), startColumn);
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
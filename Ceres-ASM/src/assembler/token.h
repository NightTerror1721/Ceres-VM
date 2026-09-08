#pragma once

#include "common_defs.h"
#include "identifier.h"
#include "data_type.h"
#include "strings_pool.h"
#include <string>
#include <compare>
#include <memory>
#include <variant>

namespace ceres::casm
{
	enum class TokenType
	{
		Invalid = 0,

		// Identifiers
		Identifier,		// e.g., variable names, label names, etc.
		DollarIdentifier, // e.g., $localLabel, $globalLabel, etc.
		DoublePercentIdentifier, // e.g., %%localLabel, %%globalLabel, etc.

		// Literals
		LiteralInteger, // e.g., 123, 0x7B, 0b1111011
		LiteralFloat,   // e.g., 1.23, .5, 1e-3
		LiteralChar,    // e.g., 'A', 'B', etc.
		LiteralBool,    // e.g., true, false
		LiteralString,  // e.g., "Hello, World!", 'A'

		// Keywords
		Keyword,		// e.g., let, const, global

		// Data Types
		DataType,		// e.g., u8, u16, u32, string

		// Punctuation
		At,				// @ (used for sections like @data, @text, etc.)
		Colon,			// : (used for labels)
		Dot,			// . (used for local labels)
		Comma,			// , (used for separating operands)
		Equals,			// = (used for defining constants and variables)
		// Comparison and remainder, which exist so that `assert` has something to assert. `=` stays
		// the assignment in a declaration; the comparison is spelled `==`, as everywhere else.
		Percent,		// %
		EqualEqual,		// ==
		BangEqual,		// !=
		Less,			// <
		LessEqual,		// <=
		Greater,		// >
		GreaterEqual,	// >=
		Plus,			// + (used for memory address calculations and const expressions)
		Minus,			// - (used for memory address calculations and const expressions)
		Asterisk,		// * (used for const expressions)
		Slash,			// / (used for const expressions)
		BracketOpen,	// [ (used for memory access)
		BracketClose,	// ] (used for memory access)
		ParenOpen,		// ( (grouping in const expressions, and size queries)
		ParenClose,		// )

		// Control
		EndOfLine,		// End of line
		EndOfFile,		// End of file/input
	};

	class TokenPayload
	{
	private:
		std::variant<
			std::monostate,
			Identifier,
			u32,
			f32,
			char,
			bool,
			LiteralString,
			SectionType,
			DataType,
			KeywordType
		> _value;

	public:
		constexpr TokenPayload() noexcept = default;
		constexpr TokenPayload(const TokenPayload&) noexcept = default;
		constexpr TokenPayload(TokenPayload&&) noexcept = default;
		constexpr ~TokenPayload() noexcept = default;

		constexpr TokenPayload& operator=(const TokenPayload&) noexcept = default;
		constexpr TokenPayload& operator=(TokenPayload&&) noexcept = default;

		constexpr bool operator==(const TokenPayload&) const noexcept = default;

	public:
		constexpr TokenPayload(Identifier identifier) noexcept : _value(identifier) {}
		constexpr TokenPayload(u32 intValue) noexcept : _value(intValue) {}
		constexpr TokenPayload(f32 floatValue) noexcept : _value(floatValue) {}
		constexpr TokenPayload(char charValue) noexcept : _value(charValue) {}
		constexpr TokenPayload(bool boolValue) noexcept : _value(boolValue) {}
		constexpr TokenPayload(LiteralString strValue) noexcept : _value(strValue) {}
		constexpr TokenPayload(DataType dataType) noexcept : _value(dataType) {}
		constexpr TokenPayload(KeywordType keywordType) noexcept : _value(keywordType) {}

		constexpr bool hasValue() const noexcept { return !_value.valueless_by_exception() && !std::holds_alternative<std::monostate>(_value); }
		constexpr bool isIdentifier() const noexcept { return std::holds_alternative<Identifier>(_value); }
		constexpr bool isInteger() const noexcept { return std::holds_alternative<u32>(_value); }
        constexpr bool isFloat() const noexcept { return std::holds_alternative<f32>(_value); }
		constexpr bool isChar() const noexcept { return std::holds_alternative<char>(_value); }
		constexpr bool isBool() const noexcept { return std::holds_alternative<bool>(_value); }
		constexpr bool isLiteralString() const noexcept { return std::holds_alternative<LiteralString>(_value); }
		constexpr bool isDataType() const noexcept { return std::holds_alternative<DataType>(_value); }
		constexpr bool isKeywordType() const noexcept { return std::holds_alternative<KeywordType>(_value); }

		constexpr Identifier asIdentifier() const noexcept { return std::get<Identifier>(_value); }
		constexpr u32 asInteger() const noexcept { return std::get<u32>(_value); }
		constexpr f32 asFloat() const noexcept { return std::get<f32>(_value); }
		constexpr char asChar() const noexcept { return std::get<char>(_value); }
		constexpr bool asBool() const noexcept { return std::get<bool>(_value); }
		constexpr LiteralString asLiteralString() const noexcept { return std::get<LiteralString>(_value); }
		constexpr DataType asDataType() const noexcept { return std::get<DataType>(_value); }
		constexpr KeywordType asKeywordType() const noexcept { return std::get<KeywordType>(_value); }
	};

	class Token
	{
	private:
		TokenType _type = TokenType::Invalid;
		std::string_view _lexeme{};
		TokenPayload _payload{};
		u32 _line = 0;
		u32 _column = 0;

	public:
		constexpr Token() noexcept = default;
		constexpr Token(const Token&) noexcept = default;
		constexpr Token(Token&&) noexcept = default;
		constexpr ~Token() noexcept = default;

		constexpr Token& operator=(const Token&) noexcept = default;
		constexpr Token& operator=(Token&&) noexcept = default;

		constexpr bool operator==(const Token& other) const noexcept = default;

	private:
		constexpr explicit Token(TokenType type, std::string_view lexeme, TokenPayload payload, u32 line, u32 column) noexcept
			: _type(type), _lexeme(lexeme), _payload(payload), _line(line), _column(column)
		{}

	public:
		constexpr TokenType type() const noexcept { return _type; }
		constexpr std::string_view lexeme() const noexcept { return _lexeme; }
		constexpr u32 line() const noexcept { return _line; }
		constexpr u32 column() const noexcept { return _column; }

		constexpr Identifier identifierValue() const noexcept { return _payload.asIdentifier(); }
		constexpr u32 integerValue() const noexcept { return _payload.asInteger(); }
		constexpr f32 floatValue() const noexcept { return _payload.asFloat(); }
		constexpr char charValue() const noexcept { return _payload.asChar(); }
		constexpr bool boolValue() const noexcept { return _payload.asBool(); }
		constexpr LiteralString literalStringValue() const noexcept { return _payload.asLiteralString(); }
		constexpr DataType dataTypeValue() const noexcept { return _payload.asDataType(); }
		constexpr KeywordType keywordTypeValue() const noexcept { return _payload.asKeywordType(); }

		constexpr bool is(TokenType expectedType) const noexcept { return _type == expectedType; }

		constexpr bool isValid() const noexcept { return _type != TokenType::Invalid; }
		constexpr bool isInvalid() const noexcept { return _type == TokenType::Invalid; }

		constexpr bool isIdentifier() const noexcept { return _type == TokenType::Identifier; }
		constexpr bool isDollarIdentifier() const noexcept { return _type == TokenType::DollarIdentifier; }
		constexpr bool isDoublePercentIdentifier() const noexcept { return _type == TokenType::DoublePercentIdentifier; }

        constexpr bool isLiteral() const noexcept
		{
			return _type == TokenType::LiteralInteger ||
				_type == TokenType::LiteralFloat ||
				_type == TokenType::LiteralString ||
				_type == TokenType::LiteralChar ||
				_type == TokenType::LiteralBool;
		}
		constexpr bool isLiteralInteger() const noexcept { return _type == TokenType::LiteralInteger; }
        constexpr bool isLiteralFloat() const noexcept { return _type == TokenType::LiteralFloat; }
		constexpr bool isLiteralString() const noexcept { return _type == TokenType::LiteralString; }
		constexpr bool isLiteralChar() const noexcept { return _type == TokenType::LiteralChar; }
		constexpr bool isLiteralBool() const noexcept { return _type == TokenType::LiteralBool; }

		constexpr bool isPunctuation() const noexcept
		{
			switch (_type)
			{
				case TokenType::At:
				case TokenType::Colon:
				case TokenType::Dot:
				case TokenType::Comma:
				case TokenType::Equals:
				case TokenType::BracketOpen:
				case TokenType::BracketClose:
				case TokenType::ParenOpen:
				case TokenType::ParenClose:
					return true;
				default:
					return false;
			}
		}
		constexpr bool isAt() const noexcept { return _type == TokenType::At; }
		constexpr bool isColon() const noexcept { return _type == TokenType::Colon; }
		constexpr bool isDot() const noexcept { return _type == TokenType::Dot; }
		constexpr bool isComma() const noexcept { return _type == TokenType::Comma; }
		constexpr bool isEquals() const noexcept { return _type == TokenType::Equals; }
		constexpr bool isBracketOpen() const noexcept { return _type == TokenType::BracketOpen; }
		constexpr bool isBracketClose() const noexcept { return _type == TokenType::BracketClose; }
		constexpr bool isParenOpen() const noexcept { return _type == TokenType::ParenOpen; }
		constexpr bool isParenClose() const noexcept { return _type == TokenType::ParenClose; }

		constexpr bool isKeyword() const noexcept { return _type == TokenType::Keyword; }

		constexpr bool isDataType() const noexcept { return _type == TokenType::DataType; }

		constexpr bool isEndOfLine() const noexcept { return _type == TokenType::EndOfLine; }
		constexpr bool isEndOfFile() const noexcept { return _type == TokenType::EndOfFile; }
		constexpr bool isEndOfInput() const noexcept { return _type == TokenType::EndOfLine || _type == TokenType::EndOfFile; }

	public:
		static Token makeInvalid(u32 line, u32 column) noexcept { return Token{ TokenType::Invalid, {}, {}, line, column }; }
		static Token makeIdentifier(std::string_view lexeme, Identifier identifier, u32 line, u32 column) noexcept
		{
			return Token{ TokenType::Identifier, lexeme, identifier, line, column };
		}
		static Token makeDollarIdentifier(std::string_view lexeme, Identifier identifier, u32 line, u32 column) noexcept
		{
			return Token{ TokenType::DollarIdentifier, lexeme, identifier, line, column };
		}
		static Token makeDoublePercentIdentifier(std::string_view lexeme, Identifier identifier, u32 line, u32 column) noexcept
		{
			return Token{ TokenType::DoublePercentIdentifier, lexeme, identifier, line, column };
		}
		static Token makeLiteralInteger(std::string_view lexeme, u32 value, u32 line, u32 column) noexcept
		{
			return Token{ TokenType::LiteralInteger, lexeme, value, line, column };
		}
		static Token makeLiteralString(std::string_view lexeme, LiteralString value, u32 line, u32 column) noexcept
		{
			return Token{ TokenType::LiteralString, lexeme, value, line, column };
		}
		static Token makeLiteralFloat(std::string_view lexeme, float value, u32 line, u32 column) noexcept
		{
			return Token{ TokenType::LiteralFloat, lexeme, value, line, column };
		}
		static Token makeLiteralChar(std::string_view lexeme, char value, u32 line, u32 column) noexcept
		{
			return Token{ TokenType::LiteralChar, lexeme, value, line, column };
		}
		static Token makeLiteralBool(std::string_view lexeme, bool value, u32 line, u32 column) noexcept
		{
			return Token{ TokenType::LiteralBool, lexeme, value, line, column };
		}
		static Token makeKeyword(std::string_view lexeme, KeywordType keyword, u32 line, u32 column) noexcept
		{
			return Token{ TokenType::Keyword, lexeme, keyword, line, column };
		}
		static Token makeDataType(std::string_view lexeme, DataType dataType, u32 line, u32 column) noexcept
		{
			return Token{ TokenType::DataType, lexeme, dataType, line, column };
		}
		static Token makeAt(u32 line, u32 column) noexcept { return Token{ TokenType::At, "@", {}, line, column }; }
		static Token makeColon(u32 line, u32 column) noexcept { return Token{ TokenType::Colon, ":", {}, line, column }; }
		static Token makeDot(u32 line, u32 column) noexcept { return Token{ TokenType::Dot, ".", {}, line, column }; }
		static Token makeComma(u32 line, u32 column) noexcept { return Token{ TokenType::Comma, ",", {}, line, column }; }
		static Token makeEquals(u32 line, u32 column) noexcept { return Token{ TokenType::Equals, "=", {}, line, column }; }
		static Token makePercent(u32 line, u32 column) noexcept { return Token{ TokenType::Percent, "%", {}, line, column }; }
		static Token makeEqualEqual(u32 line, u32 column) noexcept { return Token{ TokenType::EqualEqual, "==", {}, line, column }; }
		static Token makeBangEqual(u32 line, u32 column) noexcept { return Token{ TokenType::BangEqual, "!=", {}, line, column }; }
		static Token makeLess(u32 line, u32 column) noexcept { return Token{ TokenType::Less, "<", {}, line, column }; }
		static Token makeLessEqual(u32 line, u32 column) noexcept { return Token{ TokenType::LessEqual, "<=", {}, line, column }; }
		static Token makeGreater(u32 line, u32 column) noexcept { return Token{ TokenType::Greater, ">", {}, line, column }; }
		static Token makeGreaterEqual(u32 line, u32 column) noexcept { return Token{ TokenType::GreaterEqual, ">=", {}, line, column }; }
		static Token makePlus(u32 line, u32 column) noexcept { return Token{ TokenType::Plus, "+", {}, line, column }; }
		static Token makeMinus(u32 line, u32 column) noexcept { return Token{ TokenType::Minus, "-", {}, line, column }; }
		static Token makeAsterisk(u32 line, u32 column) noexcept { return Token{ TokenType::Asterisk, "*", {}, line, column }; }
		static Token makeSlash(u32 line, u32 column) noexcept { return Token{ TokenType::Slash, "/", {}, line, column }; }
		static Token makeBracketOpen(u32 line, u32 column) noexcept { return Token{ TokenType::BracketOpen, "[", {}, line, column }; }
		static Token makeBracketClose(u32 line, u32 column) noexcept { return Token{ TokenType::BracketClose, "]", {}, line, column }; }
		static Token makeParenOpen(u32 line, u32 column) noexcept { return Token{ TokenType::ParenOpen, "(", {}, line, column }; }
		static Token makeParenClose(u32 line, u32 column) noexcept { return Token{ TokenType::ParenClose, ")", {}, line, column }; }
		static Token makeEndOfLine(u32 line, u32 column) noexcept { return Token{ TokenType::EndOfLine, {}, {}, line, column }; }
		static Token makeEndOfFile(u32 line, u32 column) noexcept { return Token{ TokenType::EndOfFile, {}, {}, line, column }; }
	};
}

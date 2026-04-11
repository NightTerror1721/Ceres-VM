#pragma once

#include "common_defs.h"
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

		// Literals
		LiteralInteger, // e.g., 123, 0x7B, 0b1111011
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
		Plus,			// + (used for memory address calculations and const expressions)
		Minus,			// - (used for memory address calculations and const expressions)
		Asterisk,		// * (used for const expressions)
		Slash,			// / (used for const expressions)
		BracketOpen,	// [ (used for memory access)
		BracketClose,	// ] (used for memory access)

		// Control
		EndOfFile,		// End of file/input
	};

	class TokenPayload
	{
	private:
		std::variant<std::monostate, u32, std::string, SectionType, DataType, KeywordType> _value;

	public:
		TokenPayload() noexcept = default;
		TokenPayload(const TokenPayload&) noexcept = default;
		TokenPayload(TokenPayload&&) noexcept = default;
		~TokenPayload() noexcept = default;

		TokenPayload& operator=(const TokenPayload&) noexcept = default;
		TokenPayload& operator=(TokenPayload&&) noexcept = default;

		bool operator==(const TokenPayload& other) const noexcept = default;

	public:
		TokenPayload(u32 intValue) noexcept : _value(intValue) {}
		TokenPayload(const std::string& strValue) noexcept : _value(strValue) {}
		TokenPayload(std::string&& strValue) noexcept : _value(std::move(strValue)) {}
		TokenPayload(std::string_view strValue) noexcept : _value(std::string(strValue)) {}
		TokenPayload(SectionType sectionType) noexcept : _value(sectionType) {}
		TokenPayload(DataType dataType) noexcept : _value(dataType) {}
		TokenPayload(KeywordType keywordType) noexcept : _value(keywordType) {}

		bool hasValue() const noexcept { return !_value.valueless_by_exception() && !std::holds_alternative<std::monostate>(_value); }
		bool isInteger() const noexcept { return std::holds_alternative<u32>(_value); }
		bool isString() const noexcept { return std::holds_alternative<std::string>(_value); }

		u32 asInteger() const noexcept { return std::get<u32>(_value); }
		const std::string& asString() const noexcept { return std::get<std::string>(_value); }
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
		Token() noexcept = default;
		Token(const Token&) noexcept = default;
		Token(Token&&) noexcept = default;
		~Token() noexcept = default;

		Token& operator=(const Token&) noexcept = default;
		Token& operator=(Token&&) noexcept = default;

		bool operator==(const Token& other) const noexcept = default;

	private:
		Token(TokenType type, std::string_view lexeme, TokenPayload payload, u32 line, u32 column) noexcept
			: _type(type), _lexeme(lexeme), _payload(payload), _line(line), _column(column)
		{}

	public:
		constexpr TokenType type() const noexcept { return _type; }
		constexpr std::string_view lexeme() const noexcept { return _lexeme; }
		constexpr u32 line() const noexcept { return _line; }
		constexpr u32 column() const noexcept { return _column; }

		u32 integerValue() const noexcept { return _payload.isInteger() ? _payload.asInteger() : 0; }
		std::string_view stringValue() const noexcept { return _payload.isString() ? _payload.asString() : std::string_view{}; }

		constexpr bool is(TokenType expectedType) const noexcept { return _type == expectedType; }

		constexpr bool isValid() const noexcept { return _type != TokenType::Invalid; }
		constexpr bool isInvalid() const noexcept { return _type == TokenType::Invalid; }

		constexpr bool isIdentifier() const noexcept { return _type == TokenType::Identifier; }

		constexpr bool isLiteral() const noexcept { return _type == TokenType::LiteralInteger || _type == TokenType::LiteralString; }
		constexpr bool isLiteralInteger() const noexcept { return _type == TokenType::LiteralInteger; }
		constexpr bool isLiteralString() const noexcept { return _type == TokenType::LiteralString; }

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

		constexpr bool isKeyword() const noexcept { return _type == TokenType::Keyword; }

		constexpr bool isDataType() const noexcept { return _type == TokenType::DataType; }

		constexpr bool isEndOfFile() const noexcept { return _type == TokenType::EndOfFile; }

	public:
		static Token makeInvalid(u32 line, u32 column) noexcept { return Token{ TokenType::Invalid, {}, {}, line, column }; }
		static Token makeIdentifier(std::string_view lexeme, u32 line, u32 column) noexcept
		{
			return Token{ TokenType::Identifier, lexeme, {}, line, column };
		}
		static Token makeLiteralInteger(std::string_view lexeme, u32 value, u32 line, u32 column) noexcept
		{
			return Token{ TokenType::LiteralInteger, lexeme, value, line, column };
		}
		static Token makeLiteralString(std::string_view lexeme, std::string&& value, u32 line, u32 column) noexcept
		{
			return Token{ TokenType::LiteralString, lexeme, std::move(value), line, column };
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
		static Token makePlus(u32 line, u32 column) noexcept { return Token{ TokenType::Plus, "+", {}, line, column }; }
		static Token makeMinus(u32 line, u32 column) noexcept { return Token{ TokenType::Minus, "-", {}, line, column }; }
		static Token makeAsterisk(u32 line, u32 column) noexcept { return Token{ TokenType::Asterisk, "*", {}, line, column }; }
		static Token makeSlash(u32 line, u32 column) noexcept { return Token{ TokenType::Slash, "/", {}, line, column }; }
		static Token makeBracketOpen(u32 line, u32 column) noexcept { return Token{ TokenType::BracketOpen, "[", {}, line, column }; }
		static Token makeBracketClose(u32 line, u32 column) noexcept { return Token{ TokenType::BracketClose, "]", {}, line, column }; }
		static Token makeEndOfFile(u32 line, u32 column) noexcept { return Token{ TokenType::EndOfFile, {}, {}, line, column }; }
	};
}

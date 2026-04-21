#pragma once

#include "token.h"
#include <string>
#include <functional>
#include <optional>

namespace ceres::casm
{
	class LexerSource
	{
	private:
		std::string_view _source;
		uoffset _position = 0;
		u32 _line = 1;
		u32 _column = 1;

	public:
		LexerSource() = delete;
		constexpr LexerSource(const LexerSource&) noexcept = default;
		constexpr LexerSource(LexerSource&&) noexcept = default;
		constexpr ~LexerSource() noexcept = default;

		constexpr LexerSource& operator=(const LexerSource&) noexcept = default;
		constexpr LexerSource& operator=(LexerSource&&) noexcept = default;

	public:
		constexpr explicit LexerSource(std::string_view source) noexcept : _source(source) {}

		constexpr bool isAtEnd() const noexcept { return _position >= _source.size(); }

		constexpr std::string_view source() const noexcept { return _source; }
		constexpr uoffset position() const noexcept { return _position; }
		constexpr u32 line() const noexcept { return _line; }
		constexpr u32 column() const noexcept { return _column; }

		constexpr char peek() const noexcept { return isAtEnd() ? '\0' : _source[_position]; }
		constexpr char peek(ioffset offset) const noexcept
		{
			ioffset targetPos = static_cast<ioffset>(_position) + offset;
			if (targetPos < 0 || static_cast<uoffset>(targetPos) >= _source.size())
				return '\0';
			return _source[static_cast<uoffset>(targetPos)];
		}

		constexpr char next() noexcept
		{
			if (isAtEnd())
				return '\0';

			char ch = _source[_position++];
			if (ch == '\n')
			{
				_line++;
				_column = 1;
			}
			else
			{
				_column++;
			}
			return ch;
		}
		constexpr char next(uoffset count) noexcept
		{
			char lastChar = '\0';
			for (uoffset i = 0; i < count; i++)
			{
				lastChar = next();
				if (lastChar == '\0')
					break;
			}
			return lastChar;
		}

		inline usize skipUntil(std::move_only_function<bool(char)> predicate)
		{
			usize startPos = _position;
			while (!isAtEnd())
			{
				char ch = _source[_position];
				if (predicate(ch))
					break;
				next();
			}
			return _position - startPos;
		}

		constexpr std::string_view getSourceSubpart(uoffset startOffset, uoffset length) const noexcept
		{
			if (startOffset >= _source.size())
				return std::string_view{};

			uoffset maxLength = _source.size() - startOffset;
			uoffset actualLength = length < maxLength ? length : maxLength;
			return _source.substr(startOffset, actualLength);
		}
		constexpr std::string_view peekSource(ioffset offset, uoffset length) const noexcept
		{
			ioffset targetPos = static_cast<ioffset>(_position) + offset;
			if (targetPos < 0 || static_cast<uoffset>(targetPos) >= _source.size())
				return std::string_view{};
			return getSourceSubpart(static_cast<uoffset>(targetPos), length);
		}
		constexpr std::string_view peekSource(uoffset length) const noexcept { return getSourceSubpart(_position, length); }
		constexpr std::string_view peekSource() const noexcept { return getSourceSubpart(_position, _source.size() - _position); }

		constexpr std::string_view peekSourceUntilCurrentPosition(uoffset offset) const noexcept
		{
			if (offset > _position)
				return std::string_view{};
			return _source.substr(_position - offset, offset);
		}

	public:
		constexpr explicit operator bool() const noexcept { return !isAtEnd(); }
		constexpr bool operator!() const noexcept { return isAtEnd(); }

		constexpr char operator*() const noexcept { return peek(); }
		constexpr char operator[](ioffset offset) const noexcept
		{
			if (offset == 0)
				return peek();
			else
				return peek(offset);
		}

		constexpr LexerSource& operator++() noexcept
		{
			next();
			return *this;
		}

		constexpr LexerSource& operator+=(uoffset count) noexcept
		{
			next(count);
			return *this;
		}
	};

	class Lexer
	{
	private:
		LexerSource _source;

	public:
		Lexer() = delete;
		Lexer(const Lexer&) noexcept = default;
		Lexer(Lexer&&) noexcept = default;
		~Lexer() noexcept = default;

		Lexer& operator=(const Lexer&) noexcept = default;
		Lexer& operator=(Lexer&&) noexcept = default;

	public:
		explicit Lexer(std::string_view source) noexcept : _source(source) {}

		Token nextToken();

	public:
		inline bool isAtEnd() const noexcept { return _source.isAtEnd(); }

	private:
		void skipWhitespaceAndComments() noexcept;

		Token scanIdentifierOrKeyword(usize startPosition, u32 startColumn) noexcept;
		Token scanNumberLiteral(usize startPosition, u32 startColumn) noexcept;
		Token scanStringLiteral(usize startPosition, u32 startColumn) noexcept;
		Token scanCharLiteral(usize startPosition, u32 startColumn) noexcept;

		static std::optional<KeywordType> checkKeyword(std::string_view identifier) noexcept;

	private:
		static constexpr bool isValidDigit(char ch, int base) noexcept
		{
			if (base == 10) return ch >= '0' && ch <= '9';
			if (base == 2) return ch == '0' || ch == '1';
			if (ch >= '0' && ch <= '9') return true;
			if (ch >= 'a' && ch <= 'f') return true;
			if (ch >= 'A' && ch <= 'F') return true;
			return false;
		};

		static constexpr int hexValue(unsigned char ch) noexcept
		{
			if (ch >= '0' && ch <= '9') return ch - '0';
			if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
			if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
			return -1;
		};
	};
}

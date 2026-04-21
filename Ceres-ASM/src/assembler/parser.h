#pragma once

#include "operand.h"
#include "statement.h"
#include "lexer.h"
#include "errors.h"
#include <format>

namespace ceres::casm
{
	class ParserError : public AssemblerError
	{
	private:
		u32 _line;
		u32 _column;

	public:
		using AssemblerError::AssemblerError;
	};

	class ParserCursor
	{
	private:
		Lexer _lexer;
		Token _currentToken;
		Token _peekedToken;

	public:
		ParserCursor() = delete;
		ParserCursor(const ParserCursor&) noexcept = default;
		ParserCursor(ParserCursor&&) noexcept = default;
		~ParserCursor() noexcept = default;

		ParserCursor& operator=(const ParserCursor&) noexcept = default;
		ParserCursor& operator=(ParserCursor&&) noexcept = default;

	public:
		explicit ParserCursor(std::string_view source) noexcept :
			_lexer(source),
			_currentToken(_lexer.nextToken()),
			_peekedToken(_lexer.nextToken())
		{}

		inline [[nodiscard]] const Token& current() const noexcept { return _currentToken; }
		inline [[nodiscard]] const Token& peek() const noexcept { return _peekedToken; }

		inline [[nodiscard]] bool isAtEnd() const noexcept { return _lexer.isAtEnd(); }

		inline const Token& next() noexcept
		{
			_currentToken = std::move(_peekedToken);
			_peekedToken = std::move(_lexer.nextToken());
			return _currentToken;
		}

		inline const Token& consume(TokenType expectedType, std::string_view errorMessage) 		{
			if (_currentToken.is(expectedType))
			{
				const Token& token = _currentToken;
				next(); // Consume the token after returning it
				return token;
			}
			throw ParserError(_currentToken.line(), _currentToken.column(), errorMessage);
		}
		inline const Token& consume(DataType expectedDataType, std::string_view errorMessage)
		{
			if (_currentToken.isDataType() && _currentToken.dataTypeValue() == expectedDataType)
			{
				const Token& token = _currentToken;
				next(); // Consume the token after returning it
				return token;
			}
			throw ParserError(_currentToken.line(), _currentToken.column(), errorMessage);
		}
		inline const Token& consume(KeywordType expectedKeywordType, std::string_view errorMessage) 
		{
			if (_currentToken.isKeyword() && _currentToken.keywordTypeValue() == expectedKeywordType)
			{
				const Token& token = _currentToken;
				next(); // Consume the token after returning it
				return token;
			}
			throw ParserError(_currentToken.line(), _currentToken.column(), errorMessage);
		}

		inline const Token& consumeEndOfLineOrEndOfFile(std::string_view errorMessage)
		{
			if (_currentToken.isEndOfInput())
			{
				const Token& token = _currentToken;
				next(); // Consume the token after returning it
				return token;
			}
			throw ParserError(_currentToken.line(), _currentToken.column(), errorMessage);
		}

		inline void skipUntilEndOfLineOrEndOfFile() noexcept
		{
			while (!_currentToken.isEndOfInput())
				next();
		}

		inline [[nodiscard]] bool match(TokenType expectedType) const noexcept
		{
			return _currentToken.is(expectedType);
		}
		inline [[nodiscard]] bool match(DataType expectedDataType) const noexcept
		{
			return _currentToken.isDataType() && _currentToken.dataTypeValue() == expectedDataType;
		}
		inline [[nodiscard]] bool match(KeywordType expectedKeywordType) const noexcept
		{
			return _currentToken.isKeyword() && _currentToken.keywordTypeValue() == expectedKeywordType;
		}

		inline [[nodiscard]] bool matchAny(std::initializer_list<TokenType> expectedTypes) const noexcept
		{
			for (TokenType type : expectedTypes)
			{
				if (_currentToken.is(type))
					return true;
			}
			return false;
		}
		inline [[nodiscard]] bool matchAny(std::initializer_list<DataType> expectedDataTypes) const noexcept
		{
			for (DataType dataType : expectedDataTypes)
			{
				if (_currentToken.isDataType() && _currentToken.dataTypeValue() == dataType)
					return true;
			}
			return false;
		}
		inline [[nodiscard]] bool matchAny(std::initializer_list<KeywordType> expectedKeywordTypes) const noexcept
		{
			for (KeywordType keywordType : expectedKeywordTypes)
			{
				if (_currentToken.isKeyword() && _currentToken.keywordTypeValue() == keywordType)
					return true;
			}
			return false;
		}

		inline [[nodiscard]] bool isCurrentInvalid() const noexcept
		{
			return _currentToken.isInvalid();
		}

		inline [[nodiscard]] bool isCurrentEndOfLineOrEndOfFile() const noexcept
		{
			return _currentToken.isEndOfInput();
		}
	};

	class Parser
	{
	private:
		ParserCursor _cursor;
		AssemblerErrorHandler& _errorHandler;

	public:
		Parser() = delete;
		Parser(const Parser&) noexcept = delete;
		Parser(Parser&&) noexcept = delete;
		~Parser() noexcept = default;

		Parser& operator=(const Parser&) noexcept = delete;
		Parser& operator=(Parser&&) noexcept = delete;

	public:
		explicit Parser(std::string_view source, AssemblerErrorHandler& errorHandler) noexcept :
			_cursor(source), _errorHandler(errorHandler)
		{}

		std::vector<Statement> parse();

	private:
		Statement parseSection();
		Statement parseDataDeclaration();
		Statement parseLabelOrInstruction();

		DataTypeReference parseDataType();
		LiteralValueReference parseLiteralValue(std::optional<DataTypeReference> expectedDataType);
		LiteralValueReferenceElement parseLiteralValueElement(std::optional<DataTypeScalarCode> expectedScalarCode);
		Operand parseOperand();

	private:
		[[noreturn]] void error(std::string_view message) const
		{
			const Token& token = _cursor.current();
			throw ParserError(token.line(), token.column(), message);
		}

		template <typename... Args>
		[[noreturn]] void error(std::string_view formatStr, Args&&... args) const
		{
			const Token& token = _cursor.current();
            std::string message = std::vformat(formatStr, std::make_format_args(args...));
			throw ParserError(token.line(), token.column(), message);
		}
	};
}

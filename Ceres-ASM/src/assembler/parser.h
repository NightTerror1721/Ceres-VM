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
	public:
		using AssemblerError::AssemblerError;
	};

	class ParserCursor
	{
	private:
		std::string_view _file; // Owned by the Parser that constructs this cursor, which outlives it
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
		explicit ParserCursor(std::string_view source, StringPool& stringPool, std::string_view file) noexcept :
			_file(file),
			_lexer(source, stringPool),
			_currentToken(_lexer.nextToken()),
			_peekedToken(_lexer.nextToken())
		{}

		[[nodiscard]] inline const Token& current() const noexcept { return _currentToken; }
		[[nodiscard]] inline const Token& peek() const noexcept { return _peekedToken; }

		[[nodiscard]] inline bool isAtEnd() const noexcept { return _currentToken.isEndOfFile() || (_currentToken.isInvalid() && _lexer.isAtEnd()); }

		inline Token next() noexcept
		{
			_currentToken = std::move(_peekedToken);
			_peekedToken = std::move(_lexer.nextToken());
			return _currentToken;
		}

		inline Token consume(TokenType expectedType, std::string_view errorMessage) 		{
			if (_currentToken.is(expectedType))
			{
				Token token = std::move(_currentToken);
				next(); // Consume the token after returning it
				return token;
			}
			throw ParserError(_file, _currentToken.line(), _currentToken.column(), errorMessage);
		}
		inline Token consume(DataType expectedDataType, std::string_view errorMessage)
		{
			if (_currentToken.isDataType() && _currentToken.dataTypeValue() == expectedDataType)
			{
				Token token = std::move(_currentToken);
				next(); // Consume the token after returning it
				return token;
			}
			throw ParserError(_file, _currentToken.line(), _currentToken.column(), errorMessage);
		}
		inline Token consume(KeywordType expectedKeywordType, std::string_view errorMessage) 
		{
			if (_currentToken.isKeyword() && _currentToken.keywordTypeValue() == expectedKeywordType)
			{
				Token token = std::move(_currentToken);
				next(); // Consume the token after returning it
				return token;
			}
			throw ParserError(_file, _currentToken.line(), _currentToken.column(), errorMessage);
		}

		inline Token consumeEndOfLineOrEndOfFile(std::string_view errorMessage)
		{
			if (_currentToken.isEndOfInput())
			{
				Token token = std::move(_currentToken);
				next(); // Consume the token after returning it
				return token;
			}
			throw ParserError(_file, _currentToken.line(), _currentToken.column(), errorMessage);
		}

		inline void skipUntilEndOfLineOrEndOfFile() noexcept
		{
			while (!_currentToken.isEndOfInput())
				next();
		}

		[[nodiscard]] inline bool match(TokenType expectedType) const noexcept
		{
			return _currentToken.is(expectedType);
		}
		[[nodiscard]] inline bool match(DataType expectedDataType) const noexcept
		{
			return _currentToken.isDataType() && _currentToken.dataTypeValue() == expectedDataType;
		}
		[[nodiscard]] inline bool match(KeywordType expectedKeywordType) const noexcept
		{
			return _currentToken.isKeyword() && _currentToken.keywordTypeValue() == expectedKeywordType;
		}

		[[nodiscard]] inline bool matchAny(std::initializer_list<TokenType> expectedTypes) const noexcept
		{
			for (TokenType type : expectedTypes)
			{
				if (_currentToken.is(type))
					return true;
			}
			return false;
		}
		[[nodiscard]] inline bool matchAny(std::initializer_list<DataType> expectedDataTypes) const noexcept
		{
			for (DataType dataType : expectedDataTypes)
			{
				if (_currentToken.isDataType() && _currentToken.dataTypeValue() == dataType)
					return true;
			}
			return false;
		}
		[[nodiscard]] inline bool matchAny(std::initializer_list<KeywordType> expectedKeywordTypes) const noexcept
		{
			for (KeywordType keywordType : expectedKeywordTypes)
			{
				if (_currentToken.isKeyword() && _currentToken.keywordTypeValue() == keywordType)
					return true;
			}
			return false;
		}

		[[nodiscard]] inline bool isCurrentInvalid() const noexcept
		{
			return _currentToken.isInvalid();
		}

		[[nodiscard]] inline bool isCurrentEndOfLineOrEndOfFile() const noexcept
		{
			return _currentToken.isEndOfInput();
		}
	};

	class Parser
	{
	private:
		std::string_view _file; // View into AssemblyState's interned path storage; empty if none was given
		ParserCursor _cursor;
		StringPool& _stringPool;
		AssemblerErrorHandler& _errorHandler;

	public:
		Parser() = delete;
		Parser(const Parser&) noexcept = delete;
		Parser(Parser&&) noexcept = delete;
		~Parser() noexcept = default;

		Parser& operator=(const Parser&) noexcept = delete;
		Parser& operator=(Parser&&) noexcept = delete;

	public:
		explicit Parser(std::string_view source, StringPool& stringPool, AssemblerErrorHandler& errorHandler, std::string_view file = {}) noexcept :
			_file(file), _cursor(source, stringPool, _file), _stringPool(stringPool), _errorHandler(errorHandler)
		{}

		std::vector<Statement> parse();

	private:
		Optional<Statement> parseStatement();

		Statement parseSection();
		Statement parseDataDeclaration();
		Statement parseImportDeclaration();
		Statement parseLabelOrInstruction();
		Statement parseMacroLabel();
		Statement parseMacroDeclaration();

		DataTypeReference parseDataType();
		LiteralValueReference parseLiteralValue(std::optional<DataTypeReference> expectedDataType);
		LiteralValueReferenceElement parseLiteralValueElement(std::optional<DataTypeScalarCode> expectedScalarCode);
		Operand parseOperand();

		// Constant expressions over integer literals, with the usual precedence. Folded here
		// because the parser is the only place that sees the operator tokens; identifiers are not
		// foldable yet, since a constant is not resolved until the translation unit is built.
		u32 parseConstantExpression();
		u32 parseConstantTerm();
		u32 parseConstantFactor();
		bool atConstantOperator() const noexcept;

	private:
		[[noreturn]] void error(std::string_view message) const
		{
			const Token& token = _cursor.current();
			throw ParserError(_file, token.line(), token.column(), message);
		}

		template <typename... Args>
		[[noreturn]] void error(std::string_view formatStr, Args&&... args) const
		{
			const Token& token = _cursor.current();
            std::string message = std::vformat(formatStr, std::make_format_args(args...));
			throw ParserError(_file, token.line(), token.column(), message);
		}
	};
}

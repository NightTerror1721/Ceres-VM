#pragma once

#include "operand.h"
#include "statement.h"
#include "lexer.h"
#include "errors.h"
#include <format>
#include <unordered_map>

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
		// `alias cursor = r5`. Purely lexical, and resolved here rather than in the symbol table:
		// by the time anything downstream sees the operand it is already an ordinary register, so
		// nothing else in the pipeline has to know registers can be named. That also makes it
		// file-scoped - a parser has no imports to consult, they are resolved a stage later.
		std::unordered_map<std::string, Operand> _registerAliases;
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
		// `isGlobal` is set by the caller when a 'global' prefix was consumed ahead of the keyword.
		Statement parseDataDeclaration(bool isGlobal);
		Statement parseImportDeclaration();
		Statement parseLabelOrInstruction();
		Statement parseMacroLabel();
		Statement parseMacroDeclaration(bool isGlobal);
		Statement parseStructDeclaration(bool isGlobal);
		void parseRegisterAlias();
		bool atQualifiedName() const noexcept;
		Identifier parseQualifiedName();

		DataTypeReference parseDataType();
		LiteralValueReference parseLiteralValue(std::optional<DataTypeReference> expectedDataType);
		std::vector<LiteralValueReferenceElement> parseLiteralGroup();
		Operand parseOperand();
		Operand makeImmediateOperand(ConstExpr&& expression);

		// Constant expressions, with the usual precedence, kept as a tree rather than folded on the
		// spot. The parser sees the operator tokens but not the constants - those are not defined
		// until the translation unit is built - so folding here is what made `BASE + 4` impossible.
		ConstExpr parseConstExpr();
		ConstExpr parseConstTerm();
		ConstExpr parseConstFactor();
		bool atConstantOperator() const noexcept;

		// True when the current identifier token starts sizeof(...), countof(...) or dimof(...).
		bool atConstantQuery() const noexcept;

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

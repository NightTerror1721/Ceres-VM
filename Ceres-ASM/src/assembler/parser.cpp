#include "parser.h"

namespace ceres::casm
{
	std::vector<Statement> Parser::parse()
	{
		std::vector<Statement> statements;

		while (!_cursor.isAtEnd())
		{
			try
			{
				if (_cursor.isCurrentInvalid())
				{
					_cursor.next(); // Skip the invalid token
					error("Unexpected invalid token");
				}

				if (_cursor.match(TokenType::At))
				{
					statements.push_back(parseSection());
					_cursor.consumeEndOfLineOrEndOfFile("Expected end of line or end of file after section declaration");
					continue;
				}

				if (_cursor.match(TokenType::Keyword))
				{
					if (_cursor.match(KeywordType::Global))
						statements.push_back(parseLabelOrInstruction());
					else if (_cursor.matchAny({ KeywordType::Let, KeywordType::Constant }))
						statements.push_back(parseDataDeclaration());
					else
						error("Unexpected keyword {}", _cursor.current().lexeme());
					_cursor.consumeEndOfLineOrEndOfFile("Expected comma between operands or end of line after statement");
					continue;
				}

				if (_cursor.matchAny({ TokenType::Identifier, TokenType::Dot }))
				{
					statements.push_back(parseLabelOrInstruction());
					_cursor.consumeEndOfLineOrEndOfFile("Expected end of line or end of file after statement");
					continue;
				}

				error("Unexpected token {}", _cursor.current().lexeme());
			}
			catch (const ParserError& error)
			{
				_errorHandler.reportError(error);
				_cursor.skipUntilEndOfLineOrEndOfFile();
				_cursor.next(); // Move to the next token after skipping
			}
		}

		return statements;
	}

	Statement Parser::parseSection()
	{
		u32 line = _cursor.consume(TokenType::At, "Expected '@' for section declaration").line();
		const Token& sectionToken = _cursor.consume(TokenType::Identifier, "Expected section name after '@'");

		std::string_view sectionName = sectionToken.lexeme();
		if (sectionName == "text") 
			return Statement::makeSection(line, SectionType::Text);
		else if (sectionName == "data") 
			return Statement::makeSection(line, SectionType::Data);
		else if (sectionName == "rodata")
			return Statement::makeSection(line, SectionType::Rodata);
		else if (sectionName == "bss") 
			return Statement::makeSection(line, SectionType::BSS);
		else
			error("Unknown section name '{}'", sectionName);

		return Statement{}; // This line will never be reached, but is added to satisfy the compiler
	}

	Statement Parser::parseDataDeclaration()
	{
		u32 line = _cursor.current().line();
		KeywordType keyword = _cursor.current().keywordTypeValue();
		_cursor.next(); // Consume 'let' or 'const'

		if (keyword != KeywordType::Let && keyword != KeywordType::Constant)
			error("Expected 'let' or 'const' keyword for data declaration");

		bool isConstant = keyword == KeywordType::Constant;

		const Token& identifierToken = _cursor.consume(TokenType::Identifier, "Expected identifier after 'let' or 'const'");
		std::string_view name = identifierToken.lexeme();

		std::optional<DataTypeInfo> dataTypeInfo = std::nullopt;
		if (!isConstant || _cursor.match(TokenType::Colon))
		{
			_cursor.consume(TokenType::Colon, "Expected ':' after identifier in data declaration");

			dataTypeInfo = parseDataType();
			if (!dataTypeInfo->isValid())
				error("Invalid data type specified in data declaration");
		}

		std::optional<LiteralValue> initialValue = std::nullopt;
		if (_cursor.match(TokenType::Equals))
		{
			_cursor.next(); // Consume '='
			initialValue = parseLiteralValue(dataTypeInfo, false);
		}
		else if (isConstant)
			error("Expected '=' and initializer for constant data declaration");

		return Statement::makeData(line, isConstant, Identifier::make(name), std::move(dataTypeInfo), std::move(initialValue));
	}

	Statement Parser::parseLabelOrInstruction()
	{
		u32 line = _cursor.current().line();
		LabelLevel labelLevel = LabelLevel::File;
		if (_cursor.match(KeywordType::Global))
		{
			labelLevel = LabelLevel::Global;
			_cursor.next(); // Consume 'global' keyword
		}
		else if (_cursor.match(TokenType::Dot))
		{
			labelLevel = LabelLevel::Local;
			_cursor.next(); // Consume '.' for local label
		}

		const Token& identifierToken = _cursor.consume(TokenType::Identifier, "Expected identifier for label or instruction");

		if (_cursor.match(TokenType::Colon))
		{
			_cursor.next(); // Consume ':'
			return Statement::makeLabel(line, Identifier::make(identifierToken.lexeme()), labelLevel);
		}

		if (labelLevel != LabelLevel::File)
			error("Global or local label specifier must be followed by a label declaration");

		std::optional<Mnemonic> mnemonic = stringToMnemonic(identifierToken.lexeme(), false);
		if (!mnemonic.has_value())
			error("Unknown instruction mnemonic '{}'", identifierToken.lexeme());

		std::vector<Operand> operands;

		while (!_cursor.isCurrentEndOfLineOrEndOfFile())
		{
			operands.push_back(parseOperand());
			if (!_cursor.match(TokenType::Comma))
				break;
			_cursor.next(); // Consume ',' and continue parsing operands
		}

		_cursor.consumeEndOfLineOrEndOfFile("Expected end of line or end of file after instruction");

		return Statement::makeInstruction(line, *mnemonic, std::move(operands));
	}

	DataTypeInfo Parser::parseDataType()
	{
		const Token& dataTypeToken = _cursor.consume(TokenType::DataType, "Expected data type after ':' in data declaration");
		DataType dataType = dataTypeToken.dataTypeValue();
		if (_cursor.match(TokenType::BracketOpen))
		{
			_cursor.next(); // Consume '['
			if (_cursor.match(TokenType::LiteralInteger))
			{
				if (!isScalarDataType(dataType))
					error("Array size can only be specified for scalar data types");

				const Token& arraySizeToken = _cursor.current();
				if (arraySizeToken.integerValue() == 0)
					error("Array size cannot be zero");

				_cursor.next(); // Consume array size token
				_cursor.consume(TokenType::BracketClose, "Expected ']' after array size in data declaration");
				return DataTypeInfo::makeSizedArray(dataType, arraySizeToken.integerValue());
			}
			else if (_cursor.match(TokenType::Identifier))
			{
				if (!isScalarDataType(dataType))
					error("Array size can only be specified for scalar data types");

				const Token& arraySizeToken = _cursor.current();
				std::string_view arraySizeIdentifierName = arraySizeToken.lexeme();
				if (!Identifier::isValidIdentifierName(arraySizeIdentifierName))
					error("Invalid identifier used for array size in data declaration");

				_cursor.next(); // Consume array size token
				_cursor.consume(TokenType::BracketClose, "Expected ']' after array size in data declaration");
				return DataTypeInfo::makeSizedArray(dataType, Identifier::make(arraySizeIdentifierName));
			}
			else if (_cursor.match(TokenType::BracketClose))
			{
				_cursor.next(); // Consume ']'
				return DataTypeInfo::makeUnsizedArray(dataType);
			}
			else
			{
				error("Expected array size (literal integer or identifier) or ']' for unsized array in data declaration");
			}
		}
		else
		{
			if (dataType == DataType::String)
				return DataTypeInfo::makeUnsizedArray(dataType);
			else
				return DataTypeInfo::makeScalar(dataTypeToken.dataTypeValue());
		}
	}

	LiteralValue Parser::parseLiteralValue(std::optional<DataTypeInfo> expectedDataType, bool nestedContext)
	{
		const Token& token = _cursor.current();
		_cursor.next(); // Consume the token

		LiteralValue literalValue;
		switch (token.type())
		{
			case TokenType::Identifier:
				literalValue = LiteralValue::makeIdentifier(Identifier::make(token.lexeme()));
				break;

			case TokenType::LiteralInteger:
				literalValue = LiteralValue::makeInteger(token.integerValue());
				break;

			case TokenType::LiteralFloat:
				literalValue = LiteralValue::makeFloat(token.floatValue());
				break;

			case TokenType::LiteralChar:
				literalValue = LiteralValue::makeChar(token.charValue());
				break;

			case TokenType::LiteralBool:
				literalValue = LiteralValue::makeBool(token.boolValue());
				break;

			case TokenType::LiteralString:
				literalValue = LiteralValue::makeString(token.stringValue());
				break;

			case TokenType::BracketOpen:
			{
				if (nestedContext)
					error("Nested arrays are not allowed");

				std::optional<DataTypeInfo> expectedElementType = std::nullopt;
				if (expectedDataType.has_value())
				{
					if (!expectedDataType->isUnsizedArray() && !expectedDataType->isSizedArray())
						error("Expected a scalar literal value, but got an array literal");

					DataType elementType = expectedDataType->type();
					if (!isScalarDataType(elementType))
						error("Expected a scalar literal value, but got an array literal");
					expectedElementType = DataTypeInfo::makeScalar(expectedDataType->type());
				}

				std::vector<LiteralValue> arrayElements;
				while (!_cursor.match(TokenType::BracketClose))
				{
					LiteralValue element = parseLiteralValue(expectedElementType, true);
					arrayElements.push_back(std::move(element));
					if (_cursor.match(TokenType::Comma))
						_cursor.next(); // Consume ',' and continue parsing elements
					else if (!_cursor.match(TokenType::BracketClose))
						error("Expected ',' or ']' in array literal");
				}
				_cursor.consume(TokenType::BracketClose, "Expected ']' to close array literal");
				literalValue = LiteralValue::makeArray(std::move(arrayElements));
			}
			break;

			default:
				error("Unexpected token {} in literal value", token.lexeme());
		}

		if (expectedDataType.has_value() && !expectedDataType->matchLiteralValue(literalValue, true))
			error("Expected a literal value of type {}, but got a different type", expectedDataType->toString());

		return literalValue;
	}

	Operand Parser::parseOperand()
	{
		const Token& token = _cursor.current();

		// Handle memory operand (e.g., [r1], [r2 + 4], etc.)
		if (_cursor.match(TokenType::BracketOpen))
		{
			_cursor.next(); // Consume '['
			const Token& baseRegToken = _cursor.consume(TokenType::Identifier, "Expected register identifier after '[' for memory operand");
			const auto baseReg = RegisterInfo::get(baseRegToken.lexeme());
			if (!baseReg.has_value())
				error("Invalid register '{}' for memory operand", baseRegToken.lexeme());
			if (!baseReg->isFloatingPoint)
				error("Base register for memory operand must be a general-purpose register, not a floating-point register");

			if (_cursor.match(TokenType::BracketClose))
			{
				_cursor.next(); // Consume ']'
				return Operand::makeMemory(baseReg->index);
			}

			bool isMinus = _cursor.match(TokenType::Minus);
			if (!isMinus && !_cursor.match(TokenType::Plus))
				error("Expected '+' or '-' after base register in memory operand");
			_cursor.next(); // Consume '+' or '-'

			Operand memOp;
			if (_cursor.match(TokenType::LiteralInteger))
			{
				u32 offset = _cursor.current().integerValue();
				if (isMinus)
					offset = static_cast<u32>(-static_cast<i32>(offset));
				memOp = Operand::makeMemory(baseReg->index, offset);
				_cursor.next(); // Consume the integer literal
			}
			else if (_cursor.match(TokenType::Identifier))
			{
				memOp = Operand::makeMemory(baseReg->index, Identifier::make(_cursor.current().lexeme()));
				_cursor.next(); // Consume the identifier
			}
			else
				error("Expected integer literal or identifier after '+' or '-' in memory operand");

			_cursor.consume(TokenType::BracketClose, "Expected ']' to close memory operand");
			return memOp;
		}

		// Handle register operand or identifier operand
		if (_cursor.match(TokenType::Identifier))
		{
			const Token& regToken = _cursor.current();
			const auto regInfo = RegisterInfo::get(regToken.lexeme());
			if (regInfo.has_value())
			{
				_cursor.next(); // Consume the register identifier
				return regInfo->isFloatingPoint
					? Operand::makeFloatingPointRegister(regInfo->index)
					: Operand::makeRegister(regInfo->index);
			}

			_cursor.next(); // Consume the register identifier
			return Operand::makeIdentifier(Identifier::make(regToken.lexeme()));
		}

		// Handle immediate operand (literal integer)
		if (_cursor.match(TokenType::LiteralInteger))
		{
			u32 value = _cursor.current().integerValue();
			_cursor.next(); // Consume the literal integer
			return Operand::makeImmediate(value);
		}

		error("Unexpected token {} in operand", _cursor.current().lexeme());
	}
}
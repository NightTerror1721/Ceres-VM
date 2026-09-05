#include "parser.h"

namespace ceres::casm
{
	std::vector<Statement> Parser::parse()
	{
		std::vector<Statement> statements;

		while (!_cursor.isAtEnd())
		{
			Optional<Statement> statement = parseStatement();
			if (statement.has_value())
				statements.push_back(std::move(statement.value()));
		}

		return statements;
	}

	Optional<Statement> Parser::parseStatement()
	{
		try
		{
			if (_cursor.match(TokenType::EndOfLine))
			{
				_cursor.next(); // Skip empty lines
				return std::nullopt;
			}

			if (_cursor.isCurrentInvalid())
			{
				_cursor.next(); // Skip the invalid token
				error("Unexpected invalid token");
			}

			if (_cursor.match(TokenType::At))
			{
				Statement statement = parseSection();
				_cursor.consumeEndOfLineOrEndOfFile("Expected end of line or end of file after section declaration");
				return std::move(statement);
			}

			if (_cursor.match(TokenType::Keyword))
			{
				Optional<Statement> statement;
				if (_cursor.match(KeywordType::Global))
					statement = parseLabelOrInstruction();
				else if (_cursor.matchAny({ KeywordType::Let, KeywordType::Constant }))
					statement = parseDataDeclaration();
				else if (_cursor.match(KeywordType::Import))
					statement = parseImportDeclaration();
				else if (_cursor.match(KeywordType::Macro))
					statement = parseMacroDeclaration();
				else
					error("Unexpected keyword {}", _cursor.current().lexeme());
				_cursor.consumeEndOfLineOrEndOfFile("Expected comma between operands or end of line after statement");
				return std::move(statement);
			}

			if (_cursor.matchAny({ TokenType::Identifier, TokenType::Dot }))
			{
				Optional<Statement> statement = parseLabelOrInstruction();
				_cursor.consumeEndOfLineOrEndOfFile("Expected end of line or end of file after statement");
				return std::move(statement);
			}

			if (_cursor.match(TokenType::DoublePercentIdentifier))
			{
				Statement statement = parseMacroLabel();
				_cursor.consumeEndOfLineOrEndOfFile("Expected end of line or end of file after macro label");
				return std::move(statement);
			}

			error("Unexpected token {}", _cursor.current().lexeme());
		}
		catch (const ParserError& error)
		{
			_errorHandler.reportError(error);
			_cursor.skipUntilEndOfLineOrEndOfFile();
			_cursor.next(); // Move to the next token after skipping
			return std::nullopt;
		}
	}

	Statement Parser::parseSection()
	{
		u32 line = _cursor.consume(TokenType::At, "Expected '@' for section declaration").line();
		Token sectionToken = _cursor.consume(TokenType::Identifier, "Expected section name after '@'");

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

		Token identifierToken = _cursor.consume(TokenType::Identifier, "Expected identifier after 'let' or 'const'");
		std::string_view name = identifierToken.lexeme();

		std::optional<DataTypeReference> dataType = std::nullopt;
		if (_cursor.match(TokenType::Colon))
		{
			_cursor.consume(TokenType::Colon, "Expected ':' after identifier in data declaration");

			dataType = parseDataType();
			if (!dataType->isValid())
				error("Invalid data type specified in data declaration");
		}

		std::optional<LiteralValueReference> initialValue = std::nullopt;
		if (_cursor.match(TokenType::Equals))
		{
			_cursor.next(); // Consume '='
			initialValue = parseLiteralValue(dataType);
		}
		else if (isConstant)
			error("Expected '=' and initializer for constant data declaration");

		return Statement::makeData(line, isConstant, name, std::move(dataType), std::move(initialValue));
	}

	Statement Parser::parseImportDeclaration()
	{
		u32 line = _cursor.current().line();
		_cursor.consume(KeywordType::Import, "Expected 'import' keyword for import declaration");

		Token moduleNameToken = _cursor.consume(TokenType::LiteralString, "Expected module name after 'import' keyword");
		std::string_view moduleName = moduleNameToken.stringValue();
		return Statement::makeImport(line, moduleName);
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

		Token identifierToken = _cursor.consume(TokenType::Identifier, "Expected identifier for label or instruction");

		if (_cursor.match(TokenType::Colon))
		{
			_cursor.next(); // Consume ':'
			return Statement::makeLabel(line, identifierToken.lexeme(), labelLevel);
		}

		if (labelLevel != LabelLevel::File)
			error("Global or local label specifier must be followed by a label declaration");

		std::vector<Operand> operands;

		while (!_cursor.isCurrentEndOfLineOrEndOfFile())
		{
			operands.push_back(parseOperand());
			if (!_cursor.match(TokenType::Comma))
				break;
			_cursor.next(); // Consume ',' and continue parsing operands
		}

		std::optional<Mnemonic> mnemonic = stringToMnemonic(identifierToken.lexeme(), false);
		if (mnemonic.has_value())
			return Statement::makeInstruction(line, *mnemonic, std::move(operands));

		// If the identifier is not a known mnemonic, treat it as a macro call
		return Statement::makeMacroCall(line, identifierToken.lexeme(), std::move(operands));
	}

	Statement Parser::parseMacroLabel()
	{
		u32 line = _cursor.current().line();
		_cursor.consume(TokenType::Dot, "Expected '.' for macro label declaration");

		Token identifierToken = _cursor.consume(TokenType::DoublePercentIdentifier, "Expected identifier for macro label name");
		std::string_view macroLabelName = identifierToken.stringValue();
		if (!_cursor.current().isEndOfFile())
			error("Expected end of line or end of file after macro label declaration");

		return Statement::makeMacroLabel(line, macroLabelName);
	}

	Statement Parser::parseMacroDeclaration()
	{
		u32 line = _cursor.current().line();
		_cursor.consume(KeywordType::Macro, "Expected 'macro' keyword for macro declaration");

		Token identifierToken = _cursor.consume(TokenType::Identifier, "Expected identifier for macro name");

		std::string_view macroName = identifierToken.lexeme();
		std::vector<std::string> parameters;

		while (!_cursor.isCurrentEndOfLineOrEndOfFile())
		{
			Token paramToken = _cursor.consume(TokenType::DollarIdentifier, "Expected identifier for macro parameter");
			parameters.push_back(std::string(paramToken.stringValue()));
		}

		if (!_cursor.current().isEndOfFile())
			error("Expected end of line or end of file after macro parameter list");

		std::vector<Statement> bodyStatements;
		bool endOfMacroFound = false;

		while (!endOfMacroFound && !_cursor.isCurrentEndOfLineOrEndOfFile())
		{
			if (_cursor.match(KeywordType::EndMacro))
			{
				endOfMacroFound = true;
				_cursor.next(); // Consume 'endmacro' keyword
				break;
			}

			Optional<Statement> statement = parseStatement();
			if (statement.has_value())
				bodyStatements.push_back(std::move(statement.value()));
		}

		if (!endOfMacroFound)
			error("Expected 'endmacro' keyword to close macro declaration");

		return Statement::makeMacroDeclaration(line, macroName, std::move(parameters), std::move(bodyStatements));
	}

	DataTypeReference Parser::parseDataType()
	{
		Token dataTypeToken = _cursor.consume(TokenType::DataType, "Expected data type after ':' in data declaration");
		DataType dataType = dataTypeToken.dataTypeValue();
		if (!dataType.isValid())
			error("Invalid data type specified in data declaration");

		if (_cursor.match(TokenType::BracketOpen))
		{
			if (!dataType.isScalar())
				error("Array size can only be specified for scalar data types (non string types)");

			_cursor.next(); // Consume '['
			if (_cursor.match(TokenType::LiteralInteger))
			{
				Token arraySizeToken = _cursor.consume(TokenType::LiteralInteger, "Expected literal integer for array size in data declaration");
				if (arraySizeToken.integerValue() == 0)
					error("Array size cannot be zero");

				_cursor.consume(TokenType::BracketClose, "Expected ']' after array size in data declaration");
				return dataType.withNumElements(arraySizeToken.integerValue());
			}
			else if (_cursor.match(TokenType::Identifier))
			{
				Token arraySizeToken = _cursor.consume(TokenType::Identifier, "Expected identifier for array size in data declaration");
				std::string_view arraySizeIdentifierName = arraySizeToken.lexeme();
				if (!isValidIdentifierName(arraySizeIdentifierName))
					error("Invalid identifier used for array size in data declaration");

				_cursor.next(); // Consume array size token
				_cursor.consume(TokenType::BracketClose, "Expected ']' after array size in data declaration");
				return DataTypeReference::make(dataType.scalarCode(), arraySizeIdentifierName);
			}
			else if (_cursor.match(TokenType::BracketClose))
			{
				_cursor.next(); // Consume ']'
				return dataType.asUnsizedArray();
			}
			else
			{
				error("Expected array size (literal integer or identifier) or ']' for unsized array in data declaration");
			}
		}
		else
		{
			if (dataType.isUnsizedArray()) // string type only
				return dataType;
			else
				return dataType.asScalar();
		}
	}

	LiteralValueReference Parser::parseLiteralValue(std::optional<DataTypeReference> expectedDataType)
	{
		Token token = _cursor.current();
		_cursor.next(); // Consume the token

		LiteralValueReference literalValue;
		switch (token.type())
		{
			case TokenType::Identifier:
				literalValue = LiteralValueReference::makeIdentifier(token.lexeme());
				break;

			case TokenType::LiteralInteger:
				literalValue = LiteralValueReference::makeU32(token.integerValue());
				break;

			case TokenType::LiteralFloat:
				literalValue = LiteralValueReference::makeF32(token.floatValue());
				break;

			case TokenType::LiteralChar:
				literalValue = LiteralValueReference::makeChar(token.charValue());
				break;

			case TokenType::LiteralBool:
				literalValue = LiteralValueReference::makeBool(token.boolValue());
				break;

			case TokenType::LiteralString:
				literalValue = LiteralValueReference::makeString(token.stringValue());
				break;

			case TokenType::BracketOpen:
			{
				std::optional<DataTypeScalarCode> expectedElementType = std::nullopt;
				if (expectedDataType.has_value())
					expectedElementType = expectedDataType->scalarCode();

				std::vector<LiteralValueReference::ElementType> arrayElements;
				while (!_cursor.match(TokenType::BracketClose))
				{
					LiteralValueReferenceElement element = parseLiteralValueElement(expectedElementType);
					arrayElements.push_back(std::move(element));
					if (_cursor.match(TokenType::Comma))
						_cursor.next(); // Consume ',' and continue parsing elements
					else if (!_cursor.match(TokenType::BracketClose))
						error("Expected ',' or ']' in array literal");
				}
				_cursor.consume(TokenType::BracketClose, "Expected ']' to close array literal");

				literalValue = LiteralValueReference::make(std::move(arrayElements));
			}
			break;

			default:
				error("Unexpected token {} in literal value", token.lexeme());
		}

		if (expectedDataType.has_value() && !literalValue.matchDataType(*expectedDataType))
			error("Expected a literal value of type {}, but got a different type", expectedDataType->toString());

		return literalValue;
	}

	LiteralValueReferenceElement Parser::parseLiteralValueElement(std::optional<DataTypeScalarCode> expectedScalarCode)
	{
		Token token = _cursor.current();
		_cursor.next(); // Consume the token

		switch (token.type())
		{
			case TokenType::Identifier:
				return LiteralValueReferenceElement(token.lexeme());

			case TokenType::LiteralInteger:
				if (expectedScalarCode.has_value() && !DataType::isIntegerScalarCode(*expectedScalarCode))
					error("Expected a literal value of type {}, but got an integer literal", DataType::scalarCodeToString(*expectedScalarCode));
				return LiteralValueReferenceElement(LiteralScalar::make(token.integerValue()));

			case TokenType::LiteralFloat:
				if (expectedScalarCode.has_value() && *expectedScalarCode != DataTypeScalarCode::F32)
					error("Expected a literal value of type {}, but got a float literal", DataType::scalarCodeToString(*expectedScalarCode));
				return LiteralValueReferenceElement(LiteralScalar::make(token.floatValue()));

			case TokenType::LiteralChar:
				if (expectedScalarCode.has_value() && *expectedScalarCode != DataTypeScalarCode::U8)
					error("Expected a literal value of type {}, but got a char literal", DataType::scalarCodeToString(*expectedScalarCode));
				return LiteralValueReferenceElement(LiteralScalar::make(token.charValue()));

			case TokenType::LiteralBool:
				if (expectedScalarCode.has_value() && *expectedScalarCode != DataTypeScalarCode::U8)
					error("Expected a literal value of type {}, but got a bool literal", DataType::scalarCodeToString(*expectedScalarCode));
				return LiteralValueReferenceElement(LiteralScalar::make(token.boolValue()));

			default:
				error("Unexpected token {} in literal array value element", token.lexeme());
		}
	}

	Operand Parser::parseOperand()
	{
		Token token = _cursor.current();

		// Handle memory operand (e.g., [r1], [r2 + 4], etc.)
		if (_cursor.match(TokenType::BracketOpen))
		{
			_cursor.next(); // Consume '['
			Token baseRegToken = _cursor.consume(TokenType::Identifier, "Expected register identifier after '[' for memory operand");
			const auto baseReg = RegisterInfo::get(baseRegToken.lexeme());
			if (!baseReg.has_value())
				error("Invalid register '{}' for memory operand", baseRegToken.lexeme());
			if (baseReg->isFloatingPoint)
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
				memOp = Operand::makeMemory(baseReg->index, _cursor.current().lexeme());
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
			Token regToken = _cursor.current();
			const auto regInfo = RegisterInfo::get(regToken.lexeme());
			if (regInfo.has_value())
			{
				_cursor.next(); // Consume the register identifier
				return regInfo->isFloatingPoint
					? Operand::makeFloatingPointRegister(regInfo->index)
					: Operand::makeRegister(regInfo->index);
			}

			_cursor.next(); // Consume the register identifier
			return Operand::makeIdentifier(regToken.lexeme(), false);
		}

		// Handle macro parameter operand (e.g., $param)
		if (_cursor.match(TokenType::DollarIdentifier))
		{
			Token macroParamToken = _cursor.current();
			_cursor.next(); // Consume the macro parameter identifier
			return Operand::makeMacroParameter(macroParamToken.stringValue());
		}

		// Handle macro label operand (e.g., %%label)
		if (_cursor.match(TokenType::DoublePercentIdentifier))
		{
			Token macroLabelToken = _cursor.current();
			_cursor.next(); // Consume the macro label identifier
			return Operand::makeMacroLabel(macroLabelToken.stringValue());
		}

		// Handle immediate operand (literal integer)
		if (_cursor.match(TokenType::LiteralInteger))
		{
			u32 value = _cursor.current().integerValue();
			_cursor.next(); // Consume the literal integer
			return Operand::makeImmediate(value);
		}

		if (_cursor.match(TokenType::Dot) && _cursor.peek().isIdentifier())
		{
			Token localLabelToken = _cursor.peek();
			_cursor.next(); // Consume '.'
			_cursor.next(); // Consume the identifier
			return Operand::makeIdentifier(localLabelToken.lexeme(), true);
		}

		error("Unexpected token {} in operand", _cursor.current().lexeme());
	}
}
#include "parser.h"
#include "const_expr_eval.h"

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
				{
					// 'global' is a visibility prefix on any declaration, so what follows decides
					// which one this is. A label is the case where nothing follows it.
					const Token& next = _cursor.peek();
					const bool prefixesDeclaration = next.isKeyword() &&
						(next.keywordTypeValue() == KeywordType::Let ||
						 next.keywordTypeValue() == KeywordType::Constant ||
						 next.keywordTypeValue() == KeywordType::Macro);

					if (prefixesDeclaration)
					{
						const KeywordType declaration = next.keywordTypeValue();
						_cursor.next(); // Consume 'global'; the declaration keyword is current now
						statement = declaration == KeywordType::Macro
							? parseMacroDeclaration(true)
							: parseDataDeclaration(true);
					}
					else
						statement = parseLabelOrInstruction();
				}
				else if (_cursor.matchAny({ KeywordType::Let, KeywordType::Constant }))
					statement = parseDataDeclaration(false);
				else if (_cursor.match(KeywordType::Import))
					statement = parseImportDeclaration();
				else if (_cursor.match(KeywordType::Macro))
					statement = parseMacroDeclaration(false);
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
			return Statement::makeSection(_file, line, SectionType::Text);
		else if (sectionName == "data")
			return Statement::makeSection(_file, line, SectionType::Data);
		else if (sectionName == "rodata")
			return Statement::makeSection(_file, line, SectionType::Rodata);
		else if (sectionName == "bss")
			return Statement::makeSection(_file, line, SectionType::BSS);
		else
			error("Unknown section name '{}'", sectionName);

		return Statement{}; // This line will never be reached, but is added to satisfy the compiler
	}

	Statement Parser::parseDataDeclaration(bool isGlobal)
	{
		u32 line = _cursor.current().line();
		KeywordType keyword = _cursor.current().keywordTypeValue();
		_cursor.next(); // Consume 'let' or 'const'

		if (keyword != KeywordType::Let && keyword != KeywordType::Constant)
			error("Expected 'let' or 'const' keyword for data declaration");

		bool isConstant = keyword == KeywordType::Constant;

		Token identifierToken = _cursor.consume(TokenType::Identifier, "Expected identifier after 'let' or 'const'");
		Identifier name = identifierToken.identifierValue();

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

		return Statement::makeData(
			_file,
			line,
			isConstant,
			isGlobal,
			name,
			dataType.value_or(DataTypeReference::Invalid),
			initialValue.value_or(LiteralValueReference::makeEmpty()));
	}

	Statement Parser::parseImportDeclaration()
	{
		u32 line = _cursor.current().line();
		_cursor.consume(KeywordType::Import, "Expected 'import' keyword for import declaration");

		Token moduleNameToken = _cursor.consume(TokenType::LiteralString, "Expected module name after 'import' keyword");
		LiteralString moduleName = moduleNameToken.literalStringValue();
		return Statement::makeImport(_file, line, moduleName);
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
			return Statement::makeLabel(_file, line, identifierToken.identifierValue(), labelLevel);
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
			return Statement::makeInstruction(_file, line, *mnemonic, std::move(operands));

		// If the identifier is not a known mnemonic, treat it as a macro call
		return Statement::makeMacroCall(_file, line, identifierToken.identifierValue(), std::move(operands));
	}

	Statement Parser::parseMacroLabel()
	{
		u32 line = _cursor.current().line();

		// Reached with the %%identifier as the current token; there is no leading dot.
		Token identifierToken = _cursor.consume(TokenType::DoublePercentIdentifier, "Expected identifier for macro label name");
		_cursor.consume(TokenType::Colon, "Expected ':' after macro label declaration");

		return Statement::makeMacroLabel(_file, line, identifierToken.identifierValue());
	}
	Statement Parser::parseMacroDeclaration(bool isGlobal)
	{
		u32 line = _cursor.current().line();
		_cursor.consume(KeywordType::Macro, "Expected 'macro' keyword for macro declaration");

		Token identifierToken = _cursor.consume(TokenType::Identifier, "Expected identifier for macro name");
		Identifier macroName = identifierToken.identifierValue();

		std::vector<Identifier> parameters;
		while (!_cursor.isCurrentEndOfLineOrEndOfFile())
		{
			Token paramToken = _cursor.consume(TokenType::DollarIdentifier, "Expected $identifier for macro parameter");
			parameters.push_back(paramToken.identifierValue());

			if (_cursor.match(TokenType::Comma))
				_cursor.next();
		}

		// The parameter list ends the header line; the body runs until endmacro.
		_cursor.consumeEndOfLineOrEndOfFile("Expected end of line after macro parameter list");

		std::vector<Statement> bodyStatements;
		bool endOfMacroFound = false;

		while (!_cursor.isAtEnd())
		{
			if (_cursor.match(TokenType::EndOfLine))
			{
				_cursor.next();
				continue;
			}

			if (_cursor.match(KeywordType::EndMacro))
			{
				_cursor.next();
				endOfMacroFound = true;
				break;
			}

			Optional<Statement> statement = parseStatement();
			if (statement.has_value())
				bodyStatements.push_back(std::move(statement.value()));
		}

		if (!endOfMacroFound)
			error("Expected 'endmacro' to close the declaration of macro '{}'", macroName.view());

		return Statement::makeMacroDeclaration(_file, line, isGlobal, macroName, std::move(parameters), std::move(bodyStatements));
	}
	DataTypeReference Parser::parseDataType()
	{
		Token dataTypeToken = _cursor.consume(TokenType::DataType, "Expected data type after ':' in data declaration");
		DataType dataType = dataTypeToken.dataTypeValue();
		if (!dataType.isValid())
			error("Invalid data type specified in data declaration");

		if (!_cursor.match(TokenType::BracketOpen))
		{
			if (dataType.isUnsizedArray()) // the `string` alias, which is u8[] spelled differently
				return DataTypeReference::make(dataType);
			return DataTypeReference::makeScalar(dataType.scalarCode(), dataType.alias());
		}

		if (!dataType.isScalar())
			error("Array size can only be specified for scalar data types (non string types)");

		// Any dimension may be left empty, not just the outermost: a size is only an error when the
		// initialiser cannot supply it, which is not something the parser can know yet.
		std::vector<DataTypeReference::Dimension> dimensions;
		while (_cursor.match(TokenType::BracketOpen))
		{
			_cursor.next(); // Consume '['

			if (_cursor.match(TokenType::BracketClose))
			{
				_cursor.next(); // Consume ']'
				dimensions.emplace_back(std::nullopt);
			}
			else
			{
				ConstExpr size = parseConstExpr();
				_cursor.consume(TokenType::BracketClose, "Expected ']' after array size in data declaration");
				dimensions.emplace_back(std::move(size));
			}

			if (dimensions.size() > DataType::MaxRank)
				error("An array may have at most {} dimensions", DataType::MaxRank);
		}

		return DataTypeReference::makeArray(dataType.scalarCode(), std::move(dimensions), dataType.alias());
	}

	LiteralValueReference Parser::parseLiteralValue(std::optional<DataTypeReference> expectedDataType)
	{
		LiteralValueReference literalValue;

		if (_cursor.match(TokenType::BracketOpen))
		{
			literalValue = LiteralValueReference::make(parseLiteralGroup());
		}
		else if (_cursor.match(TokenType::LiteralString))
		{
			// A string on its own is a flat run of characters, as it has always been.
			Token token = _cursor.current();
			_cursor.next();
			literalValue = LiteralValueReference::makeString(token.literalStringValue());
		}
		else
		{
			literalValue = LiteralValueReference::makeExpression(parseConstExpr());
		}

		// Only worth checking when both sides are plain enough to compare; anything with a nested
		// group, a constant or an inferred size is settled in TranslationUnitBuilder.
		if (expectedDataType.has_value())
		{
			if (auto concrete = expectedDataType->toLiteralDataType(); concrete.has_value() && !literalValue.matchDataType(*concrete))
				error("Expected a literal value of type {}, but got a different type", expectedDataType->toString());
		}

		return literalValue;
	}

	// One bracketed level. Nesting is kept rather than flattened here: a dimension may be declared
	// as a constant that is not resolved yet, so the parser cannot know how many elements a row is
	// meant to hold, let alone which of them to pad.
	std::vector<LiteralValueReferenceElement> Parser::parseLiteralGroup()
	{
		_cursor.consume(TokenType::BracketOpen, "Expected '[' to open an array literal");

		std::vector<LiteralValueReferenceElement> elements;
		while (!_cursor.match(TokenType::BracketClose))
		{
			if (_cursor.isCurrentEndOfLineOrEndOfFile())
				error("Unterminated array literal: expected ']'");

			if (_cursor.match(TokenType::BracketOpen))
			{
				elements.push_back(LiteralValueReferenceElement::makeGroup(parseLiteralGroup()));
			}
			else if (_cursor.match(TokenType::LiteralString))
			{
				// A string inside an array fills a whole row, so it is one element, not many.
				Token token = _cursor.current();
				_cursor.next();
				elements.push_back(LiteralValueReferenceElement::makeString(token.literalStringValue()));
			}
			else
			{
				elements.emplace_back(parseConstExpr());
			}

			if (_cursor.match(TokenType::Comma))
				_cursor.next();
			else if (!_cursor.match(TokenType::BracketClose))
				error("Expected ',' or ']' in array literal");
		}

		_cursor.consume(TokenType::BracketClose, "Expected ']' to close array literal");
		return elements;
	}

	bool Parser::atConstantOperator() const noexcept
	{
		return _cursor.match(TokenType::Plus) || _cursor.match(TokenType::Minus) ||
			_cursor.match(TokenType::Asterisk) || _cursor.match(TokenType::Slash);
	}

	bool Parser::atConstantQuery() const noexcept
	{
		return _cursor.match(TokenType::Identifier)
			&& ConstExpr::queryFromName(_cursor.current().lexeme()).has_value()
			&& _cursor.peek().is(TokenType::ParenOpen);
	}

	ConstExpr Parser::parseConstFactor()
	{
		if (_cursor.match(TokenType::Minus))
		{
			_cursor.next();
			return ConstExpr::makeNegate(parseConstFactor());
		}

		if (_cursor.match(TokenType::Plus))
		{
			_cursor.next();
			return parseConstFactor();
		}

		if (_cursor.match(TokenType::ParenOpen))
		{
			_cursor.next();
			ConstExpr inner = parseConstExpr();
			_cursor.consume(TokenType::ParenClose, "Expected ')' to close a constant expression");
			return inner;
		}

		// sizeof / countof / dimof: the only way to get at a size that was never written down,
		// which is exactly what an inferred array dimension is.
		if (atConstantQuery())
		{
			const ConstExpr::Query query = ConstExpr::queryFromName(_cursor.current().lexeme()).value();
			const std::string_view queryName = _cursor.current().lexeme();
			_cursor.next(); // Consume the query name
			_cursor.consume(TokenType::ParenOpen, "Expected '(' after a size query");

			Token nameToken = _cursor.consume(TokenType::Identifier, "Expected a symbol name inside a size query");

			u32 dimensionIndex = 0;
			if (query == ConstExpr::Query::DimOf)
			{
				_cursor.consume(TokenType::Comma, "dimof takes a symbol and a dimension index");
				Token indexToken = _cursor.consume(TokenType::LiteralInteger, "Expected a literal dimension index in dimof");
				dimensionIndex = indexToken.integerValue();
			}
			else if (_cursor.match(TokenType::Comma))
				error("{} takes a single symbol", queryName);

			_cursor.consume(TokenType::ParenClose, "Expected ')' to close a size query");
			return ConstExpr::makeQuery(query, nameToken.identifierValue(), dimensionIndex);
		}

		if (_cursor.match(TokenType::Identifier))
		{
			Token token = _cursor.current();
			_cursor.next();
			return ConstExpr::makeIdentifier(token.identifierValue());
		}

		if (_cursor.match(TokenType::LiteralInteger))
		{
			const u32 value = _cursor.current().integerValue();
			_cursor.next();
			return ConstExpr::makeLiteral(LiteralScalar::make(value));
		}

		if (_cursor.match(TokenType::LiteralFloat))
		{
			const f32 value = _cursor.current().floatValue();
			_cursor.next();
			return ConstExpr::makeLiteral(LiteralScalar::make(value));
		}

		if (_cursor.match(TokenType::LiteralChar))
		{
			const char value = _cursor.current().charValue();
			_cursor.next();
			return ConstExpr::makeLiteral(LiteralScalar::make(value));
		}

		if (_cursor.match(TokenType::LiteralBool))
		{
			const bool value = _cursor.current().boolValue();
			_cursor.next();
			return ConstExpr::makeLiteral(LiteralScalar::make(value));
		}

		error("Expected a value in constant expression, got {}", _cursor.current().lexeme());
	}

	ConstExpr Parser::parseConstTerm()
	{
		ConstExpr value = parseConstFactor();

		while (_cursor.match(TokenType::Asterisk) || _cursor.match(TokenType::Slash))
		{
			const ConstExpr::Op op = _cursor.match(TokenType::Slash) ? ConstExpr::Op::Divide : ConstExpr::Op::Multiply;
			_cursor.next();
			value = ConstExpr::makeBinary(op, std::move(value), parseConstFactor());
		}

		return value;
	}

	ConstExpr Parser::parseConstExpr()
	{
		ConstExpr value = parseConstTerm();

		while (_cursor.match(TokenType::Plus) || _cursor.match(TokenType::Minus))
		{
			const ConstExpr::Op op = _cursor.match(TokenType::Minus) ? ConstExpr::Op::Subtract : ConstExpr::Op::Add;
			_cursor.next();
			value = ConstExpr::makeBinary(op, std::move(value), parseConstTerm());
		}

		return value;
	}

	// An expression that names nothing can be folded now; one that does has to wait for a symbol
	// table, so it travels as an operand and is replaced during resolution.
	Operand Parser::makeImmediateOperand(ConstExpr&& expression)
	{
		if (expression.isSelfContained())
		{
			auto folded = evaluateConstExpr(expression, nullptr);
			if (!folded.has_value())
				error("{}", folded.error());
			return Operand::makeImmediate(folded->asRawValue());
		}

		return Operand::makeConstExpr(std::move(expression));
	}

	Operand Parser::parseOperand()
	{
		Token token = _cursor.current();

		// Handle memory operand (e.g., [r1], [r2 + 4], etc.)
		if (_cursor.match(TokenType::BracketOpen))
		{
			_cursor.next(); // Consume '['
			Token baseRegToken = _cursor.consume(TokenType::Identifier, "Expected register identifier after '[' for memory operand");
			const auto baseReg = RegisterInfo::get(baseRegToken.identifierValue());
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
				memOp = Operand::makeMemory(baseReg->index, _cursor.current().identifierValue());
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
			const auto regInfo = RegisterInfo::get(regToken.identifierValue());
			if (regInfo.has_value())
			{
				_cursor.next(); // Consume the register identifier
				return regInfo->isFloatingPoint
					? Operand::makeFloatingPointRegister(regInfo->index)
					: Operand::makeRegister(regInfo->index);
			}

			// A bare identifier is a label or a variable and stays one. It only becomes an
			// expression when it is followed by an operator, or when it is a size query.
			if (atConstantQuery() || _cursor.peek().is(TokenType::Plus) || _cursor.peek().is(TokenType::Minus) ||
				_cursor.peek().is(TokenType::Asterisk) || _cursor.peek().is(TokenType::Slash))
				return makeImmediateOperand(parseConstExpr());

			_cursor.next(); // Consume the identifier
			return Operand::makeIdentifier(regToken.identifierValue(), false);
		}

		// Handle macro parameter operand (e.g., $param)
		if (_cursor.match(TokenType::DollarIdentifier))
		{
			Token macroParamToken = _cursor.current();
			_cursor.next(); // Consume the macro parameter identifier
			return Operand::makeMacroParameter(macroParamToken.identifierValue());
		}

		// Handle macro label operand (e.g., %%label)
		if (_cursor.match(TokenType::DoublePercentIdentifier))
		{
			Token macroLabelToken = _cursor.current();
			_cursor.next(); // Consume the macro label identifier
			return Operand::makeMacroLabel(macroLabelToken.identifierValue());
		}

		// Handle immediate operand: a literal, or an expression over constants and size queries.
		if (_cursor.match(TokenType::LiteralInteger) || _cursor.match(TokenType::LiteralChar) || _cursor.match(TokenType::ParenOpen))
			return makeImmediateOperand(parseConstExpr());

		if (_cursor.match(TokenType::Dot) && _cursor.peek().isIdentifier())
		{
			Token localLabelToken = _cursor.peek();
			_cursor.next(); // Consume '.'
			_cursor.next(); // Consume the identifier
			return Operand::makeIdentifier(localLabelToken.identifierValue(), true);
		}

		error("Unexpected token {} in operand", _cursor.current().lexeme());
	}
}
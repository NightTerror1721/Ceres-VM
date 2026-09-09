#include <ceres/asm/parser.h>
#include <ceres/asm/const_expr_eval.h>

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

		// Anonymous strings and floats, gathered as they were met and declared at the end in their
		// own `.rodata`. Appending rather than interleaving keeps the sections the file wrote in
		// the order it wrote them.
		if (!_literalPool.empty())
		{
			statements.push_back(Statement::makeSection(_file, 0, SectionType::Rodata));
			for (auto& literal : _literalPool)
				statements.push_back(std::move(literal));
			_literalPool.clear();
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
						 next.keywordTypeValue() == KeywordType::Macro ||
						 next.keywordTypeValue() == KeywordType::Struct ||
						 next.keywordTypeValue() == KeywordType::Alias);

					if (prefixesDeclaration)
					{
						const KeywordType declaration = next.keywordTypeValue();
						_cursor.next(); // Consume 'global'; the declaration keyword is current now
						if (declaration == KeywordType::Macro)
							statement = parseMacroDeclaration(true);
						else if (declaration == KeywordType::Struct)
							statement = parseStructDeclaration(true);
						else if (declaration == KeywordType::Alias)
						{
							parseRegisterAlias(true);
							_cursor.consumeEndOfLineOrEndOfFile("Expected end of line after a register alias");
							return std::nullopt;
						}
						else
							statement = parseDataDeclaration(true);
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
				else if (_cursor.match(KeywordType::Struct))
					statement = parseStructDeclaration(false);
				else if (_cursor.matchAny({ KeywordType::Align, KeywordType::Org, KeywordType::Assert }))
					return parseDirective();
				else if (_cursor.match(KeywordType::Alias))
				{
					parseRegisterAlias(false);
					_cursor.consumeEndOfLineOrEndOfFile("Expected end of line after a register alias");
					return std::nullopt; // Nothing reaches the AST: the name is substituted at its use.
				}
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

	Statement Parser::parseStructDeclaration(bool isGlobal)
	{
		u32 line = _cursor.current().line();
		_cursor.consume(KeywordType::Struct, "Expected 'struct' keyword for struct declaration");

		Token nameToken = _cursor.consume(TokenType::Identifier, "Expected a name after 'struct'");
		_cursor.consumeEndOfLineOrEndOfFile("Expected end of line after a struct name");

		std::vector<StructFieldDeclaration> fields;
		bool closed = false;

		while (!_cursor.isAtEnd())
		{
			if (_cursor.match(TokenType::EndOfLine))
			{
				_cursor.next();
				continue;
			}

			if (_cursor.match(KeywordType::EndStruct))
			{
				_cursor.next();
				closed = true;
				break;
			}

			Token fieldToken = _cursor.consume(TokenType::Identifier, "Expected a field name in a struct");
			_cursor.consume(TokenType::Colon, "Expected ':' after a struct field name");

			DataTypeReference fieldType = parseDataType();
			if (!fieldType.isValid())
				error("Invalid data type for field '{}'", fieldToken.lexeme());

			fields.push_back(StructFieldDeclaration{ fieldToken.identifierValue(), std::move(fieldType) });
			_cursor.consumeEndOfLineOrEndOfFile("Expected end of line after a struct field");
		}

		if (!closed)
			error("Expected 'endstruct' to close the declaration of struct '{}'", nameToken.lexeme());
		if (fields.empty())
			error("Struct '{}' has no fields", nameToken.lexeme());

		return Statement::makeStructDeclaration(_file, line, isGlobal, nameToken.identifierValue(), std::move(fields));
	}

	// `alias cursor = r5`, and from here on `cursor` is r5 everywhere a register can be written.
	// `align 16`, `org 64`, `assert <expr>` and `assert <expr>, "why"`. All three take a constant
	// expression, which is the whole reason the expression grammar grew comparisons.
	Operand Parser::poolLiteral(LiteralValueReference&& value, DataTypeReference&& type)
	{
		// The name cannot collide with anything a program can write - '$' never appears in a
		// parsed identifier - and it must not contain a dot either, because a dot is what tells
		// a qualified `module.name` from a plain one.
		const std::string name = std::format("lit${}", _nextLiteralIndex++);
		const Identifier identifier = _stringPool.makeIdentifier(name);

		_literalPool.push_back(Statement::makeData(_file, _cursor.current().line(),
			false, false, identifier, std::move(type), std::move(value)));

		return Operand::makeIdentifier(identifier, false);
	}

	Statement Parser::parseDirective()
	{
		const u32 line = _cursor.current().line();
		const KeywordType keyword = _cursor.current().keywordTypeValue();
		_cursor.next(); // Consume the directive

		const auto kind = keyword == KeywordType::Align ? DirectiveStatement::Kind::Align
			: keyword == KeywordType::Org ? DirectiveStatement::Kind::Org
			: DirectiveStatement::Kind::Assert;

		ConstExpr value = parseConstExpr();

		std::optional<LiteralString> message;
		if (kind == DirectiveStatement::Kind::Assert && _cursor.match(TokenType::Comma))
		{
			_cursor.next(); // Consume ','
			Token messageToken = _cursor.consume(TokenType::LiteralString, "Expected a message in quotes after ',' in assert");
			message = messageToken.literalStringValue();
		}

		return Statement::makeDirective(_file, line, kind, std::move(value), message);
	}

	void Parser::parseRegisterAlias(bool isGlobal)
	{
		_cursor.consume(KeywordType::Alias, "Expected 'alias' keyword");

		Token nameToken = _cursor.consume(TokenType::Identifier, "Expected a name after 'alias'");
		const std::string name{ nameToken.lexeme() };

		if (RegisterInfo::get(nameToken.identifierValue()).has_value())
			error("'{}' is already a register name", name);

		_cursor.consume(TokenType::Equals, "Expected '=' in a register alias");

		Token registerToken = _cursor.consume(TokenType::Identifier, "Expected a register after '=' in a register alias");
		const auto registerInfo = RegisterInfo::get(registerToken.identifierValue());
		if (!registerInfo.has_value())
			error("'{}' is not a register", registerToken.lexeme());

		Operand operand = registerInfo->isFloatingPoint
			? Operand::makeFloatingPointRegister(registerInfo->index)
			: Operand::makeRegister(registerInfo->index);

		// Saying the same thing twice is not a mistake, and it happens for a good reason: a file
		// that declares the convention's own names and also imports the module that publishes
		// them is writing one alias, not two. Only a name that would come to mean a *different*
		// register is an error.
		if (const auto existing = _registerAliases.find(name); existing != _registerAliases.end())
		{
			const bool sameRegister = existing->second.isRegister() == operand.isRegister() &&
				(operand.isRegister()
					? existing->second.asRegister().regIndex == operand.asRegister().regIndex
					: existing->second.asFloatingPointRegister().regIndex == operand.asFloatingPointRegister().regIndex);
			if (!sameRegister)
				error("'{}' is already an alias for another register", name);
			return;
		}

		_registerAliases.emplace(name, operand);

		// `global alias` is collected before this file is parsed, not read out of it afterwards -
		// see Assembler::collectGlobalAliases. Nothing to record here.
		(void)isGlobal;
	}

	Statement Parser::parseImportDeclaration()
	{
		u32 line = _cursor.current().line();
		_cursor.consume(KeywordType::Import, "Expected 'import' keyword for import declaration");

		Token moduleNameToken = _cursor.consume(TokenType::LiteralString, "Expected module name after 'import' keyword");
		LiteralString moduleName = moduleNameToken.literalStringValue();

		// `as` is matched as an ordinary identifier rather than made a keyword, so it stays usable
		// as a name everywhere else in the language.
		NullableIdentifier alias = nullptr;
		if (_cursor.match(TokenType::Identifier) && _cursor.current().lexeme() == "as")
		{
			_cursor.next(); // Consume 'as'
			Token aliasToken = _cursor.consume(TokenType::Identifier, "Expected a name after 'as' in an import");
			alias = aliasToken.identifierValue();
		}

		return Statement::makeImport(_file, line, moduleName, alias);
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

		if (labelLevel == LabelLevel::File && atQualifiedName())
		{
			// Only a macro can be called by a qualified name; an instruction is never one.
			Identifier qualified = parseQualifiedName();
			std::vector<Operand> qualifiedArguments;
			while (!_cursor.isCurrentEndOfLineOrEndOfFile())
			{
				qualifiedArguments.push_back(parseOperand());
				if (!_cursor.match(TokenType::Comma))
					break;
				_cursor.next();
			}
			return Statement::makeMacroCall(_file, line, qualified, std::move(qualifiedArguments));
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
		// A struct's name in the place a type goes. A struct reserves no storage of its own - what
		// it declares is one offset constant per field plus its own name holding the total size -
		// so `Entity` is exactly `u8[Entity]`, which is what it expands to. Whether the name really
		// is a struct is the builder's to say; the parser has no tables yet.
		if (_cursor.match(TokenType::Identifier))
		{
			Token structNameToken = _cursor.current();
			_cursor.next();

			std::vector<DataTypeReference::Dimension> outerCounts;
			while (_cursor.match(TokenType::BracketOpen))
			{
				_cursor.next(); // Consume '['
				if (_cursor.match(TokenType::BracketClose))
				{
					_cursor.next(); // Consume ']'
					outerCounts.emplace_back(std::nullopt);
				}
				else
				{
					ConstExpr size = parseConstExpr();
					_cursor.consume(TokenType::BracketClose, "Expected ']' after array size in data declaration");
					outerCounts.emplace_back(std::move(size));
				}

				// The struct itself takes the innermost one, so the counts get one fewer.
				if (outerCounts.size() + 1 > DataType::MaxRank)
					error("An array may have at most {} dimensions", DataType::MaxRank);
			}

			return DataTypeReference::makeStructNamed(std::move(outerCounts), structNameToken.identifierValue());
		}

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

		if (atQualifiedName())
			return ConstExpr::makeIdentifier(parseQualifiedName());

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

		while (_cursor.match(TokenType::Asterisk) || _cursor.match(TokenType::Slash) || _cursor.match(TokenType::Percent))
		{
			const ConstExpr::Op op = _cursor.match(TokenType::Slash) ? ConstExpr::Op::Divide
				: _cursor.match(TokenType::Percent) ? ConstExpr::Op::Modulo
				: ConstExpr::Op::Multiply;
			_cursor.next();
			value = ConstExpr::makeBinary(op, std::move(value), parseConstFactor());
		}

		return value;
	}

	// Sums, which bind tighter than a comparison and looser than a product.
	ConstExpr Parser::parseConstSum()
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

	// A comparison answers 1 or 0 and binds loosest, so `a % 4 == 0` groups the way it reads.
	// Not chained: `a < b < c` would compare a boolean against c, which never means what it
	// looks like, so exactly one comparison is allowed.
	ConstExpr Parser::parseConstExpr()
	{
		ConstExpr value = parseConstSum();

		static constexpr std::pair<TokenType, ConstExpr::Op> comparisons[] = {
			{ TokenType::EqualEqual, ConstExpr::Op::Equal },
			{ TokenType::BangEqual, ConstExpr::Op::NotEqual },
			{ TokenType::LessEqual, ConstExpr::Op::LessEqual },
			{ TokenType::GreaterEqual, ConstExpr::Op::GreaterEqual },
			{ TokenType::Less, ConstExpr::Op::Less },
			{ TokenType::Greater, ConstExpr::Op::Greater },
		};

		for (const auto& [token, op] : comparisons)
		{
			if (!_cursor.match(token))
				continue;

			_cursor.next();
			return ConstExpr::makeBinary(op, std::move(value), parseConstSum());
		}

		return value;
	}

	// `math.PI`: the current token is the module name and a dot follows it *immediately*.
	//
	// Adjacency is the only thing separating `math.PI` from `jnz .loop`, a mnemonic followed by a
	// local label - the lexer throws the space away, so the columns are what is left to read it by.
	bool Parser::atQualifiedName() const noexcept
	{
		if (!_cursor.match(TokenType::Identifier) || !_cursor.peek().is(TokenType::Dot))
			return false;

		const Token& name = _cursor.current();
		const Token& dot = _cursor.peek();
		return dot.line() == name.line() && dot.column() == name.column() + static_cast<u32>(name.lexeme().size());
	}

	Identifier Parser::parseQualifiedName()
	{
		Token moduleToken = _cursor.consume(TokenType::Identifier, "Expected a module name");
		_cursor.consume(TokenType::Dot, "Expected '.' in a qualified name");
		Token nameToken = _cursor.consume(TokenType::Identifier, "Expected a name after '.' in a qualified name");
		return _stringPool.makeIdentifier(std::format("{}.{}", moduleToken.lexeme(), nameToken.lexeme()));
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

	// An integer register written where a displacement would go, by its own name or by an alias.
	// Nothing else can index: a float register holds no address, and a constant is a displacement.
	std::optional<u8> Parser::indexRegisterOf(const Token& token) const
	{
		if (const auto reg = RegisterInfo::get(token.identifierValue()); reg.has_value())
			return reg->isFloatingPoint ? std::nullopt : std::optional<u8>{ reg->index };

		if (const auto alias = _registerAliases.find(std::string(token.lexeme())); alias != _registerAliases.end())
		{
			if (alias->second.isFloatingPointRegister())
				return std::nullopt;
			return alias->second.asRegister().regIndex;
		}

		return std::nullopt;
	}

	Operand Parser::parseOperand()
	{
		Token token = _cursor.current();

		// `u8[r2 + 4]`: the access width written on the access rather than in the mnemonic, which
		// is what lets `ldr` cover what ldrb/ldrh/ldrsb/ldrsh cover. A base register alone cannot
		// say how wide the access is - unlike a variable, which has a declared type - so this is
		// where the missing half of the information goes.
		std::optional<DataTypeScalarCode> accessType;
		if (_cursor.match(TokenType::DataType) && _cursor.peek().is(TokenType::BracketOpen))
		{
			const DataType accessDataType = _cursor.current().dataTypeValue();
			if (!accessDataType.isScalar())
				error("An access type must be a scalar: '{}' is not", accessDataType.toString());

			accessType = accessDataType.scalarCode();
			_cursor.next(); // Consume the type, leaving the '[' for the memory operand below
		}

		// Handle memory operand (e.g., [r1], [r2 + 4], etc.)
		if (_cursor.match(TokenType::BracketOpen))
		{
			_cursor.next(); // Consume '['

			// `[$base + 4]`: inside a macro body the base can be a parameter, and which register
			// it is is decided by the argument at expansion. Everything else about the operand -
			// the displacement, the index, the access type - is written here and parses now.
			NullableIdentifier baseParameter = nullptr;
			if (_cursor.match(TokenType::DollarIdentifier))
			{
				baseParameter = _cursor.current().identifierValue();
				_cursor.next();
			}

			Token baseRegToken = baseParameter.isNull()
				? _cursor.consume(TokenType::Identifier, "Expected register, symbol or macro parameter after '[' for memory operand")
				: Token{};

			// `[counter]`: a symbol inside brackets means its contents, the way brackets mean the
			// contents of an address everywhere else. The address of it is the name on its own.
			// There is nothing to add to it, so a displacement or an index would be a second
			// thing the one instruction cannot do.
			if (baseParameter.isNull() &&
				!RegisterInfo::get(baseRegToken.identifierValue()).has_value() &&
				!_registerAliases.contains(std::string(baseRegToken.lexeme())))
			{
				if (!_cursor.match(TokenType::BracketClose))
					error("'[{}]' is a whole variable; it takes no displacement or index", baseRegToken.lexeme());
				_cursor.next(); // Consume ']'

				if (accessType.has_value())
					error("'[{}]' already knows its width from the variable's declared type", baseRegToken.lexeme());

				return Operand::makeDereferencedIdentifier(baseRegToken.identifierValue(), false);
			}

			u8 baseRegIndex = 0;
			bool baseIsFloat = false;
			if (!baseParameter.isNull())
			{
				// Nothing to look up yet; the argument answers it.
			}
			else if (const auto baseReg = RegisterInfo::get(baseRegToken.identifierValue()); baseReg.has_value())
			{
				baseRegIndex = baseReg->index;
				baseIsFloat = baseReg->isFloatingPoint;
			}
			else if (const auto alias = _registerAliases.find(std::string(baseRegToken.lexeme())); alias != _registerAliases.end())
			{
				// An alias names a register, so it can be a base too.
				baseIsFloat = alias->second.isFloatingPointRegister();
				baseRegIndex = baseIsFloat ? alias->second.asFloatingPointRegister().regIndex : alias->second.asRegister().regIndex;
			}
			else
				error("Invalid register '{}' for memory operand", baseRegToken.lexeme());

			if (baseIsFloat)
				error("Base register for memory operand must be a general-purpose register, not a floating-point register");

			// Everything below fills this in: the parts arrive in the order they are written.
			MemoryOperand memory;
			memory.baseRegIndex = baseRegIndex;
			memory.baseParameter = baseParameter;

			if (_cursor.match(TokenType::BracketClose))
			{
				_cursor.next(); // Consume ']'
				Operand operand = Operand::makeMemoryOperand(std::move(memory));
				return accessType.has_value() ? Operand::withAccessType(std::move(operand), accessType.value()) : operand;
			}

			bool isMinus = _cursor.match(TokenType::Minus);
			if (!isMinus && !_cursor.match(TokenType::Plus))
				error("Expected '+' or '-' after base register in memory operand");
			_cursor.next(); // Consume '+' or '-'

			if (_cursor.match(TokenType::LiteralInteger))
			{
				u32 offset = _cursor.current().integerValue();
				if (isMinus)
					offset = static_cast<u32>(-static_cast<i32>(offset));
				memory.offset = ImmediateOperand{ offset };
				_cursor.next(); // Consume the integer literal
			}
			else if (_cursor.match(TokenType::DollarIdentifier))
			{
				// `[r1 + $offset]`: a displacement, an index or a symbol, decided by the argument.
				if (isMinus)
					error("A macro parameter cannot be subtracted here; pass the negative value instead");

				memory.offset = MacroParameterOperand{ _cursor.current().identifierValue() };
				_cursor.next();
			}
			else if (atQualifiedName())
			{
				// `[r1 + Entity.y]`: a struct field offset is a constant like any other.
				memory.offset = IdentifierOperand{ parseQualifiedName() };
			}
			else if (_cursor.match(TokenType::Identifier) && indexRegisterOf(_cursor.current()).has_value())
			{
				// `[r1 + r2]` is an index, not a displacement: the second register is added at run
				// time and the instruction that does it is a different opcode.
				if (isMinus)
					error("A register index cannot be subtracted; write '[base + index]' and negate the index instead");

				memory.offset = RegisterOperand{ indexRegisterOf(_cursor.current()).value() };
				_cursor.next(); // Consume the register identifier
			}
			else if (_cursor.match(TokenType::Identifier))
			{
				memory.offset = IdentifierOperand{ _cursor.current().identifierValue() };
				_cursor.next(); // Consume the identifier
			}
			else
				error("Expected integer literal, identifier or macro parameter after '+' or '-' in memory operand");

			_cursor.consume(TokenType::BracketClose, "Expected ']' to close memory operand");
			Operand memOp = Operand::makeMemoryOperand(std::move(memory));
			return accessType.has_value() ? Operand::withAccessType(std::move(memOp), accessType.value()) : memOp;
		}

		// A type name got this far only by not being followed by '[', which is the one place a
		// type means anything in an operand.
		if (_cursor.match(TokenType::DataType))
			error("A type must be followed by a memory operand, as in u8[r1 + 4]");

		// Handle register operand or identifier operand
		if (atQualifiedName())
			return Operand::makeIdentifier(parseQualifiedName(), false);

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

			if (const auto alias = _registerAliases.find(std::string(regToken.lexeme())); alias != _registerAliases.end())
			{
				_cursor.next();
				return alias->second;
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

		// A float has 32 bits and an instruction has 16 to spare, and a string is a run of bytes
		// that is not a value at all. Both go into `.rodata` under a name nobody had to invent, and
		// the operand becomes that name - so `ldv f1, 1.5` and `la r1, "listo" ` mean what they read
		// as, instead of being rejected.
		if (_cursor.match(TokenType::LiteralFloat))
		{
			const f32 value = _cursor.current().floatValue();
			_cursor.next();
			return poolLiteral(LiteralValueReference::makeF32(value),
				DataTypeReference::makeScalar(DataTypeScalarCode::F32));
		}

		if (_cursor.match(TokenType::LiteralString))
		{
			const LiteralString text = _cursor.current().literalStringValue();
			_cursor.next();
			// One dimension, left for the initialiser to size - which is exactly what `string` is.
			std::vector<DataTypeReference::Dimension> unsized;
			unsized.emplace_back(std::nullopt);
			return poolLiteral(LiteralValueReference::makeString(text),
				DataTypeReference::makeArray(DataTypeScalarCode::U8, std::move(unsized), DataTypeAlias::String));
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
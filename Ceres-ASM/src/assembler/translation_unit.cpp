#include "translation_unit.h"
#include "instruction_info.h"
#include "assembly_state.h"
#include <array>

namespace ceres::casm
{
	void TranslationUnitBuilder::build(std::vector<Statement>&& statements)
	{
		if (_built)
			throw std::logic_error("Translation unit has already been built");

		_built = true;

		AssemblerErrorHandler& errorHandler = _translationUnit.state().errorHandler();
		_ast.reserve(statements.size());

		for (auto& statement : statements)
		{
			try
			{
				processStatement(statement, 0);
			}
			catch (const AssemblerError& error)
			{
				// SymbolTable/MacroTable errors don't know their own file (they're shared across
				// units); attribute them to the statement being processed when they were thrown.
				if (error.file().empty() && !_currentStatementFile.empty())
					errorHandler.reportError(AssemblerError(_currentStatementFile, error.line(), error.column(), error.what()));
				else
					errorHandler.reportError(error);
			}
		}

		_translationUnit.setAST(std::move(_ast));
		_translationUnit.setUnresolvedSymbols(std::move(_unresolvedSymbols));
	}

	// One statement. Recursive: the statements a macro expands into come back through here, with
	// the depth carried along so a self-referential macro is reported instead of hanging.
	void TranslationUnitBuilder::processStatement(Statement& statement, u32 expansionDepth)
	{
		// Read by error() (directly, or through any helper it calls while processing this
		// statement), so every diagnostic below is attributed to the file the statement actually
		// came from - the macro's defining file for one that came from macro expansion.
		_currentStatementFile = statement.file();

		SymbolTable& symbolTable = _translationUnit.symbolTable();
		MacroTable& macroTable = _translationUnit.macroTable();
		SectionSizes& sectionSizes = _translationUnit.sectionSizes();

		{
			{
				if (statement.isSection())
				{
					_currentSection = statement.asSection().section;
					_ast.push_back(RelocatableStatement::makeSection(statement.file(), statement.line(), std::move(statement.asSection())));
				}
				else if (statement.isLabel())
				{
					if (!_currentSection.has_value())
						error(statement.line(), "Label statement must be preceded by a section statement");

					auto& label = statement.asLabel();
					auto labelLevel = label.level;
					symbolTable.defineLabel(statement.line(), label.name, _currentSection.value(), currentOffset(), labelLevel);
					_ast.push_back(RelocatableStatement::makeLabel(statement.file(), statement.line(), currentOffset(), std::move(label)));
					if (labelLevel != LabelLevel::Local)
						_lastParentLabel = _ast.back().asLabel().name;
				}
				else if (statement.isData())
				{
					auto& data = statement.asData();
					if (!_currentSection.has_value())
					{
						if (!data.isConstant)
							error(statement.line(), "Data statement must be preceded by a section statement");
					}
					
					DataType dataType = DataType::Invalid;
					std::optional<LiteralValue> literalValue = std::nullopt;
					std::expected<u32, std::string_view> size = 0;
					if (!data.value.empty())
					{
						if (data.dataType.isValid())
						{
							auto result = resolveLiteralValue(statement.line(), data.dataType, data.value);
							dataType = result.first;
							literalValue = std::move(result.second);
							size = sizeOf(statement.line(), dataType, literalValue.value());
						}
						else
						{
							literalValue = resolveLiteralValue(statement.line(), data.value, false);
							dataType = literalValue.value().dataType();
							size = sizeOf(statement.line(), literalValue.value());
						}
					}
					else if (data.dataType.isValid())
					{
						dataType = resolveDataType(statement.line(), data.dataType, false);
						size = sizeOf(statement.line(), dataType);
					}

					if (!dataType.isValid())
						error(statement.line(), "Failed to determine data type of data statement");

					if (literalValue.has_value() && !literalValue->matchDataType(dataType))
						error(statement.line(), "Literal value does not match the specified data type");

					if (!size.has_value())
						error(statement.line(), "Failed to determine size of data statement: " + std::string(size.error()));
					if (size.value() == 0)
						error(statement.line(), "Data statement has a size of zero");

					if (data.isConstant)
					{
						if (!literalValue.has_value())
							error(statement.line(), "Constant data statement must have an initial value");

						symbolTable.defineConstant(statement.line(), data.name, data.isGlobal, literalValue.value());
					}
					else
					{
						if (!_currentSection.has_value())
							error(statement.line(), "Variable data statement must be preceded by a section statement");

						// Pad up to the element's natural alignment before recording the address, so the
					// symbol and the bytes the emitter writes agree on where the variable starts.
					const u32 padding = alignCurrentOffset(dataType.alignment());
					if (padding > 0)
					{
						switch (_currentSection.value())
						{
							case SectionType::Rodata: sectionSizes.rodataSize += padding; break;
							case SectionType::Data:   sectionSizes.dataSize += padding; break;
							case SectionType::BSS:    sectionSizes.bssSize += padding; break;
							default: break;
						}
					}

					switch (_currentSection.value())
						{
							case SectionType::Text:
								error(statement.line(), "Variable data statement cannot be in the @text section");
								break;

							case SectionType::Rodata:
								if (!literalValue.has_value())
									error(statement.line(), "Variable data statement in @rodata section must have an initial value");
								symbolTable.defineVariable(statement.line(), data.name, _currentSection.value(), currentOffset(), data.isGlobal, true, dataType, literalValue.value());
								sectionSizes.rodataSize += size.value();
								break;

							case SectionType::Data:
								if (literalValue.has_value())
									symbolTable.defineVariable(statement.line(), data.name, _currentSection.value(), currentOffset(), data.isGlobal, false, dataType, literalValue.value());
								else
									symbolTable.defineVariable(statement.line(), data.name, _currentSection.value(), currentOffset(), data.isGlobal, false, dataType);
								sectionSizes.dataSize += size.value();
								break;

							case SectionType::BSS:
								if (literalValue.has_value())
									error(statement.line(), "Variable data statement in @bss section cannot have an initial value");

								symbolTable.defineVariable(statement.line(), data.name, _currentSection.value(), currentOffset(), data.isGlobal, false, dataType);
								sectionSizes.bssSize += size.value();
								break;
						}
					}

					if (data.isConstant)
					{
						// Constant data statements do not occupy space in the program memory, so we do not increment the current offset
					}
					else
					{
						_ast.push_back(RelocatableStatement::makeData(statement.file(), statement.line(), size.value(), currentOffset(), ResolvedDataStatement{ data.isConstant, data.isGlobal, data.name, dataType, literalValue }));
						currentOffset() += size.value();
					}
				}
				else if (statement.isImport())
				{
					auto& imp = statement.asImport();
					// Relative imports resolve against the importing file, so a module travels with the
					// files it belongs to instead of depending on the working directory.
					std::filesystem::path modulePath{ imp.moduleName.str() };
					if (modulePath.is_relative() && !_sourcePath.empty())
						modulePath = _sourcePath.parent_path() / modulePath;

					const std::string resolvedPath = modulePath.lexically_normal().string();

					if (_translationUnit.state().isBeingLoaded(resolvedPath))
						error(statement.line(), "Import cycle: '{}' is already being assembled", imp.moduleName);

					auto moduleUnit = _translationUnit.state().loadTranslationUnit(resolvedPath);
					if (!moduleUnit)
						error(statement.line(), "Failed to load module '{}' (looked for {})", imp.moduleName, resolvedPath);

					// Nothing is copied out of the module: it is loaded once per run (AssemblyState
					// caches units by resolved path) and recorded here as a place to look. Importing
					// it a second time, directly or through another module, is a no-op.
					_translationUnit.addDirectImport(resolvedPath);
				}
				else if (statement.isMacroDeclaration())
				{
					MacroDeclarationStatement& macroDecl = statement.asMacroDeclaration();
					macroTable.defineMacro(std::move(macroDecl.name), macroDecl.isGlobal, std::move(macroDecl.parameters), std::move(macroDecl.body));
				}
				else if (statement.isMacroLabel())
				{
					// Only meaningful inside a macro body, where expansion turns it into a real label
					// carrying a name unique to that expansion.
					error(statement.line(), "Macro label used outside a macro body: {}", statement.asMacroLabel().name);
				}
				else if (statement.isInstruction())
				{
					if (!_currentSection.has_value() || _currentSection.value() != SectionType::Text)
						error(statement.line(), "Instruction statement must be in the @text section");

					auto& instruction = statement.asInstruction();
					for (auto& operand : instruction.operands)
						symbolTable.tryResolveOperand(statement.file(), statement.line(), operand, _lastParentLabel, _unresolvedSymbols, &_translationUnit);

					_ast.push_back(RelocatableStatement::makeInstruction(statement.file(), statement.line(), currentOffset(), std::move(instruction)));
					_ast.back().setExpansionSite(statement.expansionFile(), statement.expansionLine(), static_cast<u16>(expansionDepth));

					auto sizeOpt = InstructionInfo::findMaxSizeInBytes(instruction.mnemonic);
					if (!sizeOpt.has_value() || sizeOpt.value() == 0)
						error(statement.line(), "Failed to determine size of instruction statement");

					sectionSizes.textSize += sizeOpt.value();

					currentOffset() += sizeOpt.value();
				}
				else if (statement.isMacroCall())
				{
					// Anything that is not a known mnemonic parses as a macro call, so this is also where a
					// misspelled instruction is caught instead of being silently discarded.
					auto expanded = expandMacroCall(statement, expansionDepth);
					for (auto& expandedStatement : expanded)
						processStatement(expandedStatement, expansionDepth + 1);
				}
				else
				{
					error(statement.line(), "Unknown statement type");
				}
			}
		}
	}

	std::vector<Statement> TranslationUnitBuilder::expandMacroCall(const Statement& callStatement, u32 expansionDepth)
	{
		const u32 line = callStatement.line();
		const MacroCallStatement& call = callStatement.asMacroCall();

		if (expansionDepth >= MaxMacroExpansionDepth)
			error(line, "Macro expansion nested more than {} levels deep; '{}' is probably recursive", MaxMacroExpansionDepth, call.name);

		const auto macroOpt = _translationUnit.resolveMacro(
			MacroSignature::make(call.name.view(), static_cast<u32>(call.arity())));

		if (!macroOpt.has_value())
		{
			const auto signature = MacroSignature::make(call.name.view(), static_cast<u32>(call.arity()));
			if (const std::string_view origin = _translationUnit.findUnexportedMacroOrigin(signature); !origin.empty())
				error(line, "Macro '{}' is declared in '{}' but is not global, so it is not visible here", call.name, origin);

			error(line, "Unknown mnemonic or macro '{}' taking {} operand(s)", call.name, call.arity());
		}

		const Macro& macro = macroOpt.value().get();

		// Each expansion is numbered so that the labels it introduces cannot collide with the ones
		// from another use of the same macro.
		const u32 instanceId = ++_macroExpansionCounter;

		std::vector<Statement> expanded;
		expanded.reserve(macro.body().size());

		for (const Statement& bodyStatement : macro.body())
		{
			expanded.push_back(substituteMacroStatement(bodyStatement, macro, call, instanceId));

			// Every statement the expansion produces points back at the code the user wrote, not
			// at the macro body it was copied from. For a nested expansion the call statement's
			// own site has already been rewritten by the outer pass, so this propagates the
			// *outermost* call site all the way down rather than the immediate one.
			expanded.back().setExpansionSite(callStatement.expansionFile(), callStatement.expansionLine());
		}

		return expanded;
	}

	Statement TranslationUnitBuilder::substituteMacroStatement(const Statement& statement, const Macro& macro, const MacroCallStatement& call, u32 instanceId)
	{
		const u32 line = statement.line();

		if (statement.isMacroLabel())
			return Statement::makeLabel(statement.file(), line, makeHygienicLabel(statement.asMacroLabel().name, instanceId), LabelLevel::File);

		if (statement.isInstruction())
		{
			const InstructionStatement& instruction = statement.asInstruction();

			std::vector<Operand> operands;
			operands.reserve(instruction.operands.size());
			for (const Operand& operand : instruction.operands)
				operands.push_back(substituteMacroOperand(line, operand, macro, call, instanceId));

			return Statement::makeInstruction(statement.file(), line, instruction.mnemonic, std::move(operands));
		}

		if (statement.isMacroCall())
		{
			const MacroCallStatement& nested = statement.asMacroCall();

			std::vector<Operand> arguments;
			arguments.reserve(nested.arguments.size());
			for (const Operand& argument : nested.arguments)
				arguments.push_back(substituteMacroOperand(line, argument, macro, call, instanceId));

			return Statement::makeMacroCall(statement.file(), line, nested.name, std::move(arguments));
		}

		// Sections, labels and data declarations carry nothing to substitute.
		return statement;
	}

	Operand TranslationUnitBuilder::substituteMacroOperand(u32 line, const Operand& operand, const Macro& macro, const MacroCallStatement& call, u32 instanceId)
	{
		if (operand.isMacroParameter())
		{
			const Identifier name = operand.asMacroParameter().name;
			const auto index = macro.parameterIndex(std::string(name.view()));

			if (!index.has_value())
				error(line, "'${}' is not a parameter of macro '{}'", name, macro.name());
			if (index.value() >= call.arguments.size())
				error(line, "Macro '{}' expects {} argument(s) but was given {}", macro.name(), macro.parameterCount(), call.arguments.size());

			return call.arguments[index.value()];
		}

		if (operand.isMacroLabel())
			return Operand::makeIdentifier(makeHygienicLabel(operand.asMacroLabel().name, instanceId), false);

		return operand;
	}

	Identifier TranslationUnitBuilder::makeHygienicLabel(Identifier macroLabel, u32 instanceId)
	{
		// The generated name is not a valid identifier in the source language, so it cannot collide
		// with anything the programmer can write.
		return _translationUnit.state().stringPool().makeIdentifier(
			std::format("%%{}#{}", macroLabel.view(), instanceId));
	}

	ConstExprSymbolLookup TranslationUnitBuilder::symbolLookup() const
	{
		return [this](std::string_view name) -> const Symbol*
		{
			auto result = _translationUnit.resolveSymbol(name);
			return result.has_value() ? &result.value().get() : nullptr;
		};
	}

	u32 TranslationUnitBuilder::evaluateDimension(u32 line, const ConstExpr& expression) const
	{
		auto value = evaluateConstExpr(expression, symbolLookup());
		if (!value.has_value())
			error(line, "Array size '{}' could not be resolved: {}", expression.toString(), value.error());
		if (value->isFloat())
			error(line, "Array size '{}' is a floating point value", expression.toString());

		const i32 size = static_cast<i32>(value->asRawValue());
		if (size <= 0)
			error(line, "Array size cannot be {}", size);

		return static_cast<u32>(size);
	}

	LiteralScalar TranslationUnitBuilder::evaluateElement(u32 line, const LiteralValueReferenceElement& element, std::optional<DataTypeScalarCode> targetScalarCode) const
	{
		if (element.isGroup())
			error(line, "Expected a value here, but found a nested initialiser");

		auto value = evaluateConstExpr(element.expression(), symbolLookup());
		if (!value.has_value())
			error(line, "{}", value.error());

		// Integer literals are untyped in source: `42` carries no width of its own. When the
		// declaration states one, every element is re-tagged to it here, and only here is the value
		// checked against the width it has to fit in.
		if (!targetScalarCode.has_value() || value->scalarCode() == *targetScalarCode)
			return value.value();

		auto coerced = value->coerceTo(*targetScalarCode);
		if (!coerced.has_value())
		{
			if (!value->isInteger() || !DataType::isIntegerScalarCode(*targetScalarCode))
				error(line, "A value of type {} cannot be converted to the declared type {}",
					DataType::scalarCodeToString(value->scalarCode()), DataType::scalarCodeToString(*targetScalarCode));

			error(line, "A value does not fit in the declared type {}: it needs more than {} bits",
				DataType::scalarCodeToString(*targetScalarCode), LiteralScalar::bitWidthOf(*targetScalarCode));
		}
		return coerced.value();
	}

	void TranslationUnitBuilder::collectLiteralShape(std::span<const LiteralValueReferenceElement> elements, usize level, LiteralShape& shape)
	{
		if (shape.size() <= level)
			shape.resize(level + 1);
		shape[level].push_back(static_cast<u32>(elements.size()));

		for (const auto& element : elements)
		{
			if (element.isGroup())
				collectLiteralShape(element.group(), level + 1, shape);
		}
	}

	void TranslationUnitBuilder::flattenLiteral(u32 line, std::span<const LiteralValueReferenceElement> elements, std::span<const u32> dimensions,
		std::optional<DataTypeScalarCode> targetScalarCode, std::vector<LiteralScalar>& out) const
	{
		const u32 expected = dimensions.front();
		if (elements.size() > expected)
			error(line, "This level of the initialiser has {} elements but the declared size is {}", elements.size(), expected);

		const LiteralScalar zero = LiteralScalar::makeZero(targetScalarCode.value_or(DataTypeScalarCode::U8));

		if (dimensions.size() == 1)
		{
			for (const auto& element : elements)
				out.push_back(evaluateElement(line, element, targetScalarCode));

			// A declared dimension longer than what was written is filled with zeroes, the same way
			// a short string filling a longer array always has been.
			out.insert(out.end(), expected - elements.size(), zero);
			return;
		}

		u32 innerCount = 1;
		for (usize i = 1; i < dimensions.size(); ++i)
			innerCount *= dimensions[i];

		for (const auto& element : elements)
		{
			if (!element.isGroup())
				error(line, "Expected a nested initialiser here: the declared type has {} more dimension(s)", dimensions.size() - 1);
			flattenLiteral(line, element.group(), dimensions.subspan(1), targetScalarCode, out);
		}

		out.insert(out.end(), static_cast<usize>(expected - elements.size()) * innerCount, zero);
	}

	DataType TranslationUnitBuilder::resolveDataType(u32 line, const DataTypeReference& dataType, bool allowUnsizedArrays) const
	{
		if (!dataType.isValid())
			error(line, "Invalid data type");

		if (dataType.isScalar())
			return DataType::makeScalar(dataType.scalarCode());

		std::vector<u32> dimensions;
		dimensions.reserve(dataType.rank());
		for (const auto& dimension : dataType.dimensions())
		{
			if (!dimension.has_value())
			{
				if (!allowUnsizedArrays)
					error(line, "Without an initialiser every dimension needs a size: {} leaves one to be worked out", dataType.toString());
				return DataType::makeUnsizedArray(dataType.scalarCode());
			}
			dimensions.push_back(evaluateDimension(line, dimension.value()));
		}

		return DataType::makeArray(dataType.scalarCode(), dimensions);
	}

	LiteralValue TranslationUnitBuilder::resolveLiteralValue(u32 line, const LiteralValueReference& value, bool allowEmptyArrays, std::optional<DataTypeScalarCode> targetScalarCode) const
	{
		if (value.empty())
		{
			if (!allowEmptyArrays)
				error(line, "Empty array literal value is not allowed");
			return LiteralValue::makeEmpty();
		}

		// With no declared type there is nothing to pad up to, so the shape is whatever was written
		// and every level of it has to be regular.
		LiteralShape shape;
		collectLiteralShape(value.elements(), 0, shape);

		std::vector<u32> dimensions;
		dimensions.reserve(shape.size());
		for (usize level = 0; level < shape.size(); ++level)
		{
			const u32 first = shape[level].front();
			for (u32 length : shape[level])
			{
				if (length != first)
					error(line, "Cannot work out dimension {}: this level has rows of {} and of {} elements", level, first, length);
			}
			if (first == 0)
				error(line, "Cannot work out dimension {}: it is empty", level);
			dimensions.push_back(first);
		}

		std::vector<LiteralScalar> resolvedElements;
		flattenLiteral(line, value.elements(), dimensions, targetScalarCode, resolvedElements);

		return LiteralValue::make(std::move(resolvedElements));
	}

	std::pair<DataType, LiteralValue> TranslationUnitBuilder::resolveLiteralValue(u32 line, const DataTypeReference& expectedDataType, const LiteralValueReference& value) const
	{
		if (!expectedDataType.isValid())
			error(line, "Invalid data type");

		const DataTypeScalarCode scalarCode = expectedDataType.scalarCode();

		if (expectedDataType.isScalar())
		{
			if (value.size() != 1 || value.first().isGroup())
				error(line, "Expected a single value for a declaration of type {}", expectedDataType.toString());

			std::vector<LiteralScalar> single{ evaluateElement(line, value.first(), scalarCode) };
			return { DataType::makeScalar(scalarCode), LiteralValue::make(std::move(single)) };
		}

		LiteralShape shape;
		collectLiteralShape(value.elements(), 0, shape);

		const u8 declaredRank = expectedDataType.rank();

		// A flat list against a multidimensional declaration is allowed as long as every size is
		// written down: there is then exactly one way to cut it up.
		const bool literalIsFlat = shape.size() == 1;
		if (!literalIsFlat && shape.size() != declaredRank)
			error(line, "The initialiser is nested {} level(s) deep but {} declares {}",
				shape.size(), expectedDataType.toString(), declaredRank);

		std::vector<u32> dimensions;
		dimensions.reserve(declaredRank);
		for (u8 level = 0; level < declaredRank; ++level)
		{
			const auto& declared = expectedDataType.dimension(level);
			if (declared.has_value())
			{
				dimensions.push_back(evaluateDimension(line, declared.value()));
				continue;
			}

			if (literalIsFlat && declaredRank > 1)
				error(line, "{} needs every size written down when the initialiser is a flat list", expectedDataType.toString());

			// An omitted size is read off the initialiser, which means every row at that level has
			// to agree - an irregular one is precisely what makes the size impossible to work out.
			const u32 first = shape[level].front();
			for (u32 length : shape[level])
			{
				if (length != first)
					error(line, "Cannot work out dimension {} of {}: this level has rows of {} and of {} elements",
						level, expectedDataType.toString(), first, length);
			}
			if (first == 0)
				error(line, "Cannot work out dimension {} of {}: it is empty", level, expectedDataType.toString());
			dimensions.push_back(first);
		}

		std::vector<LiteralScalar> resolvedElements;
		if (literalIsFlat && declaredRank > 1)
		{
			u32 total = 1;
			for (u32 dimension : dimensions)
				total *= dimension;

			const std::array<u32, 1> flat{ total };
			flattenLiteral(line, value.elements(), flat, scalarCode, resolvedElements);
		}
		else
		{
			flattenLiteral(line, value.elements(), dimensions, scalarCode, resolvedElements);
		}

		return { DataType::makeArray(scalarCode, dimensions), LiteralValue::make(std::move(resolvedElements)) };
	}

	std::expected<u32, std::string_view> TranslationUnitBuilder::sizeOf(u32 line, DataType dataType) const
	{
		auto size = dataType.sizeInBytes();
		if (!size.has_value())
			error(line, "Cannot determine the size of an invalid data type {}", dataType.toString());

		return size.value();
	}

	std::expected<u32, std::string_view> TranslationUnitBuilder::sizeOf(u32 line, const LiteralValue& value) const
	{
		auto dataType = value.dataType();
		if (!dataType.isValid())
			error(line, "Cannot determine the size of an invalid literal value");

		return sizeOf(line, dataType);
	}

	std::expected<u32, std::string_view> TranslationUnitBuilder::sizeOf(u32 line, DataType dataType, const LiteralValue& value) const
	{
		if (!value.matchDataType(dataType))
			error(line, "Literal value does not match the expected data type");

		if (dataType.hasUnknownSize())
		{
			if (value.hasUnknownSize())
				error(line, "Cannot determine the size of an unsized array literal value without elements");
			return sizeOf(line, dataType.withNumElements(value.size()));
		}
		return sizeOf(line, dataType);
	}

	std::optional<std::reference_wrapper<const LiteralValue>> TranslationUnitBuilder::getConstantValue([[maybe_unused]] u32 line, std::string_view name) const noexcept
	{
		if (auto result = _translationUnit.resolveSymbol(name); result.has_value())
		{
			const Symbol& symbol = result.value().get();
			if (symbol.isConstant())
				return std::cref(symbol.value());
		}
		return std::nullopt;
	}

	// Labels already carry their own global/file/local level and are published through the linker's
	// global table, so what this governs is constants, variables and macros. A global declaration
	// stays visible however many imports it travels through; anything else never leaves the unit
	// that declares it.
	bool TranslationUnit::isExported(const Symbol& symbol) noexcept
	{
		return symbol.isGlobal() && (symbol.isConstant() || symbol.isVariable());
	}

	bool TranslationUnit::isExported(const Macro& macro) noexcept
	{
		return macro.isGlobal();
	}

	OptionalConstRef<Symbol> TranslationUnit::resolveSymbol(std::string_view name) const
	{
		if (auto own = _symbolTable.get(name); own.has_value())
			return own;

		const auto imported = lookupImportedSymbol(name);
		if (!imported.has())
			return std::nullopt;

		return std::cref(*imported.found);
	}

	OptionalConstRef<Macro> TranslationUnit::resolveMacro(const MacroSignature& signature) const
	{
		if (auto own = _macroTable.getMacro(signature); own.has_value())
			return own;

		const auto imported = lookupImportedMacro(signature);
		if (!imported.has())
			return std::nullopt;

		return std::cref(*imported.found);
	}

	TranslationUnit::ImportLookup<Symbol> TranslationUnit::lookupImportedSymbol(std::string_view name) const
	{
		ImportLookup<Symbol> result;
		std::vector<const TranslationUnit*> visited;
		visited.push_back(this);

		for (const auto& path : _directImports)
		{
			if (auto unit = state().getTranslationUnit(path); unit.has_value())
				unit->get().collectExportedSymbol(name, visited, result);
		}

		return result;
	}

	TranslationUnit::ImportLookup<Macro> TranslationUnit::lookupImportedMacro(const MacroSignature& signature) const
	{
		ImportLookup<Macro> result;
		std::vector<const TranslationUnit*> visited;
		visited.push_back(this);

		for (const auto& path : _directImports)
		{
			if (auto unit = state().getTranslationUnit(path); unit.has_value())
				unit->get().collectExportedMacro(signature, visited, result);
		}

		return result;
	}

	std::string_view TranslationUnit::findUnexportedSymbolOrigin(std::string_view name) const
	{
		std::string_view origin;
		std::vector<const TranslationUnit*> visited;
		visited.push_back(this);

		for (const auto& path : _directImports)
		{
			if (auto unit = state().getTranslationUnit(path); unit.has_value())
				unit->get().collectUnexportedSymbol(name, visited, origin);
		}

		return origin;
	}

	std::string_view TranslationUnit::findUnexportedMacroOrigin(const MacroSignature& signature) const
	{
		std::string_view origin;
		std::vector<const TranslationUnit*> visited;
		visited.push_back(this);

		for (const auto& path : _directImports)
		{
			if (auto unit = state().getTranslationUnit(path); unit.has_value())
				unit->get().collectUnexportedMacro(signature, visited, origin);
		}

		return origin;
	}

	void TranslationUnit::collectUnexportedSymbol(std::string_view name, std::vector<const TranslationUnit*>& visited, std::string_view& origin) const
	{
		if (!origin.empty() || std::find(visited.begin(), visited.end(), this) != visited.end())
			return;
		visited.push_back(this);

		if (auto own = _symbolTable.get(name); own.has_value() && !isExported(own->get()))
		{
			origin = _file;
			return;
		}

		for (const auto& path : _directImports)
		{
			if (auto unit = state().getTranslationUnit(path); unit.has_value())
				unit->get().collectUnexportedSymbol(name, visited, origin);
		}
	}

	void TranslationUnit::collectUnexportedMacro(const MacroSignature& signature, std::vector<const TranslationUnit*>& visited, std::string_view& origin) const
	{
		if (!origin.empty() || std::find(visited.begin(), visited.end(), this) != visited.end())
			return;
		visited.push_back(this);

		if (auto own = _macroTable.getMacro(signature); own.has_value() && !isExported(own->get()))
		{
			origin = _file;
			return;
		}

		for (const auto& path : _directImports)
		{
			if (auto unit = state().getTranslationUnit(path); unit.has_value())
				unit->get().collectUnexportedMacro(signature, visited, origin);
		}
	}

	void TranslationUnit::collectExportedSymbol(std::string_view name, std::vector<const TranslationUnit*>& visited, ImportLookup<Symbol>& result) const
	{
		if (std::find(visited.begin(), visited.end(), this) != visited.end())
			return;
		visited.push_back(this);

		if (auto own = _symbolTable.get(name); own.has_value() && isExported(own->get()))
		{
			const Symbol* symbol = &own->get();
			if (result.found == nullptr)
			{
				result.found = symbol;
				result.foundIn = _file;
			}
			else if (result.found != symbol && !result.ambiguous)
			{
				result.ambiguous = true;
				result.alsoIn = _file;
			}
		}

		for (const auto& path : _directImports)
		{
			if (auto unit = state().getTranslationUnit(path); unit.has_value())
				unit->get().collectExportedSymbol(name, visited, result);
		}
	}

	void TranslationUnit::collectExportedMacro(const MacroSignature& signature, std::vector<const TranslationUnit*>& visited, ImportLookup<Macro>& result) const
	{
		if (std::find(visited.begin(), visited.end(), this) != visited.end())
			return;
		visited.push_back(this);

		if (auto own = _macroTable.getMacro(signature); own.has_value() && isExported(own->get()))
		{
			const Macro* macro = &own->get();
			if (result.found == nullptr)
			{
				result.found = macro;
				result.foundIn = _file;
			}
			else if (result.found != macro && !result.ambiguous)
			{
				result.ambiguous = true;
				result.alsoIn = _file;
			}
		}

		for (const auto& path : _directImports)
		{
			if (auto unit = state().getTranslationUnit(path); unit.has_value())
				unit->get().collectExportedMacro(signature, visited, result);
		}
	}
}

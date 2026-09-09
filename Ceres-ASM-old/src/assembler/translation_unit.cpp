#include "translation_unit.h"
#include "instruction_info.h"
#include "assembly_state.h"
#include <array>
#include <algorithm>

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
							// A byte array dimensioned by a struct with an initialiser is a
							// positional struct initialiser: values map to fields in order.
							if (auto structResult = tryResolveStructLiteral(statement.line(), data.dataType, data.value);
								structResult.has_value())
							{
								dataType = structResult->first;
								literalValue = std::move(structResult->second);
								size = sizeOf(statement.line(), dataType, literalValue.value());
							}
							else
							{
								auto result = resolveLiteralValue(statement.line(), data.dataType, data.value);
								dataType = result.first;
								literalValue = std::move(result.second);
								size = sizeOf(statement.line(), dataType, literalValue.value());
							}
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
					_translationUnit.addDirectImport(resolvedPath, imp.alias.isNull() ? std::string{} : std::string(imp.alias.view()));
				}
				else if (statement.isStructDeclaration())
				{
					// A struct declares no storage. It defines one constant per field, holding that
					// field's byte offset, plus the struct's own name holding the total size - so
					// `[r1 + Entity.y]` is an ordinary constant displacement and `u8[32][Entity]`
					// is an ordinary array. Nothing downstream has to know structs exist.
					const StructDeclarationStatement& structDecl = statement.asStructDeclaration();

					u32 offset = 0;
					u32 widestAlignment = 1;
					StructLayout layout;
					layout.name = std::string(structDecl.name.view());
					layout.isGlobal = structDecl.isGlobal;
					layout.fields.reserve(structDecl.fields.size());
					for (const auto& field : structDecl.fields)
					{
						const DataType fieldType = resolveDataType(statement.line(), field.dataType, false);
						const auto fieldSize = fieldType.sizeInBytes();
						if (!fieldSize.has_value() || fieldSize.value() == 0)
							error(statement.line(), "Field '{}' of struct '{}' has no size", field.name, structDecl.name);

						const u32 alignment = fieldType.alignment();
						widestAlignment = std::max(widestAlignment, alignment);
						if (const u32 misaligned = offset % alignment; misaligned != 0)
							offset += alignment - misaligned;

						symbolTable.defineConstant(statement.line(),
							std::format("{}.{}", structDecl.name, field.name),
							structDecl.isGlobal, LiteralValue::make(offset));

						StructFieldLayout fieldLayout;
						fieldLayout.name = std::string(field.name.view());
						fieldLayout.type = fieldType;
						fieldLayout.offset = offset;
						fieldLayout.structName = std::string(structNameOf(field.dataType));
						layout.fields.push_back(std::move(fieldLayout));

						offset += fieldSize.value();
					}

					// Rounded up to the widest field, so an array of them stays aligned.
					if (const u32 misaligned = offset % widestAlignment; misaligned != 0)
						offset += widestAlignment - misaligned;

					symbolTable.defineConstant(statement.line(), structDecl.name, structDecl.isGlobal, LiteralValue::make(offset));

					layout.totalSize = offset;
					layout.widestAlignment = widestAlignment;
					_translationUnit.defineStruct(std::move(layout));
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

					auto sizeOpt = InstructionInfo::reservedSizeOf(instruction.signature());
					if (!sizeOpt.has_value() || sizeOpt.value() == 0)
						error(statement.line(), "Failed to determine size of instruction statement");

					_ast.push_back(RelocatableStatement::makeInstruction(statement.file(), statement.line(), sizeOpt.value(), currentOffset(), std::move(instruction)));
					_ast.back().setExpansionSite(statement.expansionFile(), statement.expansionLine(), static_cast<u16>(expansionDepth));

					sectionSizes.textSize += sizeOpt.value();

					currentOffset() += sizeOpt.value();
				}
				else if (statement.isDirective())
				{
					processDirective(statement, sectionSizes);
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

	void TranslationUnit::relayoutText()
	{
		Address offset = 0;
		bool inText = false;
		// Local labels are keyed as `parent.name`, so walking the AST has to keep the same running
		// parent the builder had when it defined them.
		std::string lastParentLabel;

		for (auto& statement : _ast)
		{
			if (statement.isSection())
			{
				inText = statement.asSection().section == SectionType::Text;
				continue;
			}

			if (statement.isLabel())
			{
				const LabelStatement& label = statement.asLabel();
				const bool isLocal = label.level == LabelLevel::Local;

				if (!isLocal)
					lastParentLabel = std::string(label.name.view());

				if (!inText)
					continue;

				statement.setAddress(offset);
				const std::string key = isLocal
					? string_utils::concat(lastParentLabel, ".", label.name.view())
					: std::string(label.name.view());
				_symbolTable.updateAddress(key, offset);
				continue;
			}

			if (!inText || !statement.isInstruction())
				continue;

			statement.setAddress(offset);

			// The size each statement settled on, stamped by relaxation while the operands were
			// still resolved. Recomputing it here would fall back to the mnemonic's maximum, because
			// the operands have been put back the way the first pass found them.
			offset += statement.size();
		}

		_sectionSizes.textSize = offset.value();
	}

	// `align`, `org` and `assert`, none of which puts anything in the program: the first two are
	// padding and the third is either true or a diagnostic.
	void TranslationUnitBuilder::processDirective(const Statement& statement, SectionSizes& sectionSizes)
	{
		const DirectiveStatement& directive = statement.asDirective();
		const u32 line = statement.line();

		const auto folded = evaluateConstExpr(directive.value, symbolLookup());
		if (!folded.has_value())
			error(line, "{}", folded.error());
		const i64 value = static_cast<i64>(static_cast<i32>(folded->asRawValue()));

		if (directive.kind == DirectiveStatement::Kind::Assert)
		{
			if (value != 0)
				return;

			if (directive.message.has_value())
				error(line, "Assertion failed: {}", directive.message->view());
			error(line, "Assertion failed: {}", directive.value.toString());
		}

		if (!_currentSection.has_value())
			error(line, "'{}' must appear inside a section",
				directive.kind == DirectiveStatement::Kind::Align ? "align" : "org");

		Address& offset = currentOffset();
		u32 padding = 0;

		if (directive.kind == DirectiveStatement::Kind::Align)
		{
			// A boundary that is not a power of two is almost always a typo for one that is, and
			// padding to it would quietly produce a layout nobody meant.
			if (value <= 0 || (value & (value - 1)) != 0)
				error(line, "'align' needs a positive power of two, not {}", value);

			const u32 alignment = static_cast<u32>(value);
			if (const u32 misaligned = offset.value() % alignment; misaligned != 0)
				padding = alignment - misaligned;
		}
		else
		{
			// Section-relative, and forward only: sections are placed by the linker, so an
			// absolute address is not a thing this can promise, and moving backwards would write
			// over what is already there.
			if (value < 0)
				error(line, "'org' needs an offset within the section, not {}", value);

			const u32 target = static_cast<u32>(value);
			if (target < offset.value())
				error(line, "'org {}' would move backwards: the section is already {} bytes long",
					target, offset.value());

			padding = target - offset.value();
		}

		if (padding == 0)
			return;

		offset += padding;
		switch (_currentSection.value())
		{
			case SectionType::Text: sectionSizes.textSize += padding; break;
			case SectionType::Rodata: sectionSizes.rodataSize += padding; break;
			case SectionType::Data: sectionSizes.dataSize += padding; break;
			case SectionType::BSS: sectionSizes.bssSize += padding; break;
		}

		// The padding has to exist in the emitted bytes too, and .bss has no bytes to emit.
		if (_currentSection.value() != SectionType::BSS)
			_ast.push_back(RelocatableStatement::makePadding(statement.file(), line, padding, offset - Address(padding)));
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

	// The argument a parameter stands for at this call site. Shared by the two places a parameter
	// can appear: as a whole operand, and inside a memory operand.
	const Operand& TranslationUnitBuilder::macroArgumentFor(u32 line, Identifier name, const Macro& macro, const MacroCallStatement& call) const
	{
		const auto index = macro.parameterIndex(std::string(name.view()));

		if (!index.has_value())
			error(line, "'{}' is not a parameter of macro '{}'", name, macro.name());
		if (index.value() >= call.arguments.size())
			error(line, "Macro '{}' expects {} argument(s) but was given {}", macro.name(), macro.parameterCount(), call.arguments.size());

		return call.arguments[index.value()];
	}

	Operand TranslationUnitBuilder::substituteMacroOperand(u32 line, const Operand& operand, const Macro& macro, const MacroCallStatement& call, u32 instanceId)
	{
		if (operand.isMacroParameter())
			return macroArgumentFor(line, operand.asMacroParameter().name, macro, call);

		// `[$base + 4]`, `[r1 + $offset]`: the operand around the parameter was parsed where it was
		// written; only the parts that were a parameter are filled in here. What the offset *is* -
		// a displacement, an index, or a symbol - is decided by the argument, exactly as it would
		// be if the same thing had been written out by hand.
		if (operand.isMemory() && (operand.asMemory().hasParameterBase() || operand.asMemory().isParameterOffset()))
		{
			MemoryOperand memory = operand.asMemory();

			if (memory.hasParameterBase())
			{
				const Identifier name = Identifier(memory.baseParameter);
				const Operand& argument = macroArgumentFor(line, name, macro, call);

				if (!argument.isRegister())
					error(line, "'{}' is the base of a memory operand in macro '{}', so it has to be given a general-purpose register",
						name, macro.name());

				memory.baseRegIndex = argument.asRegister().regIndex;
				memory.baseParameter = nullptr;
			}

			if (memory.isParameterOffset())
			{
				const Identifier name = memory.parameterOffset().name;
				const Operand& argument = macroArgumentFor(line, name, macro, call);

				if (argument.isImmediate())
					memory.offset = ImmediateOperand{ argument.asImmediate().value };
				else if (argument.isRegister())
					memory.offset = RegisterOperand{ argument.asRegister().regIndex };
				else if (argument.isIdentifier())
					memory.offset = IdentifierOperand{ argument.asIdentifier().name, argument.asIdentifier().isLocal };
				else
					error(line, "'{}' is the offset of a memory operand in macro '{}', so it has to be given a number, a register or a name",
						name, macro.name());
			}

			return Operand::makeMemoryOperand(std::move(memory));
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

	// An alias that promises a range has to enforce it: nothing else stops an `irq` holding 200 or a
	// `bool` holding 5, and both are mistakes worth catching where they are written.
	void TranslationUnitBuilder::checkAliasBounds(u32 line, DataTypeAlias alias, std::span<const LiteralScalar> values) const
	{
		const auto bound = DataType::aliasUpperBound(alias);
		if (!bound.has_value())
			return;

		for (const LiteralScalar& value : values)
		{
			if (value.asRawValue() > bound.value())
				error(line, "A value of {} does not fit in a '{}': the largest is {}",
					value.asRawValue(), DataType::aliasToString(alias), bound.value());
		}
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

	std::string_view TranslationUnitBuilder::structNameOf(const DataTypeReference& dataType) const
	{
		if (!dataType.isArray() || dataType.scalarCode() != DataTypeScalarCode::U8 || dataType.rank() == 0)
			return {};

		const auto& innermost = dataType.dimension(dataType.rank() - 1);
		if (!innermost.has_value() || !innermost->isIdentifier())
			return {};

		const Identifier name = innermost->name();
		return _translationUnit.resolveStruct(name.view()).has_value() ? name.view() : std::string_view{};
	}

	void TranslationUnitBuilder::checkStructNamedType(u32 line, const DataTypeReference& dataType) const
	{
		if (!dataType.isFromStructName())
			return;

		// The struct is the innermost dimension; whatever came before it are instance counts.
		const auto& structDimension = dataType.dimension(dataType.rank() - 1);
		if (!structDimension.has_value() || !structDimension->isIdentifier())
			return;

		const Identifier name = structDimension->name();
		if (_translationUnit.resolveStruct(name.view()).has_value())
			return;

		// A name that resolves to something else entirely is the mistake worth naming: a constant
		// would otherwise have been read as a byte count and nothing would have complained.
		if (_translationUnit.resolveSymbol(name.view()).has_value())
			error(line, "'{}' is not a struct, so it cannot be written as a type. For a byte array of that size, write u8[{}]", name, name);

		error(line, "Unknown type '{}'. A type is a scalar (u8, i32, f32, ...) or the name of a struct", name);
	}

	DataType TranslationUnitBuilder::resolveDataType(u32 line, const DataTypeReference& dataType, bool allowUnsizedArrays) const
	{
		if (!dataType.isValid())
			error(line, "Invalid data type");

		checkStructNamedType(line, dataType);

		if (dataType.isScalar())
			return DataType::makeScalar(dataType.scalarCode()).withAlias(dataType.alias());

		std::vector<u32> dimensions;
		dimensions.reserve(dataType.rank());
		for (const auto& dimension : dataType.dimensions())
		{
			if (!dimension.has_value())
			{
				if (!allowUnsizedArrays)
					error(line, "Without an initialiser every dimension needs a size: {} leaves one to be worked out", dataType.toString());
				return DataType::makeUnsizedArray(dataType.scalarCode()).withAlias(dataType.alias());
			}
			dimensions.push_back(evaluateDimension(line, dimension.value()));
		}

		return DataType::makeArray(dataType.scalarCode(), dimensions).withAlias(dataType.alias());
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
			checkAliasBounds(line, expectedDataType.alias(), single);
			return { DataType::makeScalar(scalarCode).withAlias(expectedDataType.alias()), LiteralValue::make(std::move(single)) };
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

		checkAliasBounds(line, expectedDataType.alias(), resolvedElements);
		return { DataType::makeArray(scalarCode, dimensions).withAlias(expectedDataType.alias()), LiteralValue::make(std::move(resolvedElements)) };
	}

	// A byte array dimensioned by a struct name, e.g. `u8[Entity]` or `u8[2][Entity]`, with an
	// initialiser. Values map to fields in declaration order; padding is zero-filled. Anything
	// else (plain sizes, constants, non-u8 element types) returns nullopt for the ordinary path.
	std::optional<std::pair<DataType, LiteralValue>> TranslationUnitBuilder::tryResolveStructLiteral(
		u32 line, const DataTypeReference& dataType, const LiteralValueReference& value) const
	{
		if (!dataType.isArray() || dataType.scalarCode() != DataTypeScalarCode::U8)
			return std::nullopt;

		// `u8[Entity] = "abc"` means those bytes. Read as fields it would silently put 'a' in the
		// first i32, 'b' in the second and the terminating zero in whatever came after.
		if (value.isFromStringLiteral())
			return std::nullopt;

		const u8 rank = dataType.rank();
		if (rank == 0)
			return std::nullopt;

		// One count and then the struct. Deeper than that, the instances would have to nest the way
		// an ordinary array's elements do - `[[a, b], [c, d]]` for `u8[2][2][S]` - and they do not:
		// the counts get multiplied and a flat run of instances is expected instead. Rather than
		// have the same shape mean two different things, this says so.
		if (rank > 2 && dataType.dimension(rank - 1).has_value() && dataType.dimension(rank - 1)->isIdentifier())
			error(line, "A struct initialiser takes at most one instance count: {} has {}", dataType.toString(), rank - 1);

		const auto& lastDim = dataType.dimension(rank - 1);
		if (!lastDim.has_value() || !lastDim->isIdentifier())
			return std::nullopt;

		auto layoutRef = _translationUnit.resolveStruct(lastDim->name().view());
		if (!layoutRef.has_value())
			return std::nullopt;
		const StructLayout& layout = layoutRef->get();

		// Naming the struct as an array size counts as a use of its size constant, exactly as
		// the ordinary `u8[Entity]` path does through evaluateDimension. Without this a struct
		// used only through an initialiser would be reported as never used.
		(void)_translationUnit.resolveSymbol(lastDim->name().view());

		// Outer dimensions are instance counts; the last one is the struct itself. A missing
		// outer size is read off the initialiser, like ordinary multidimensional arrays.
		std::vector<u32> outerCounts;
		outerCounts.reserve(rank - 1);
		bool hasInferredOuter = false;
		for (u8 level = 0; level + 1 < rank; ++level)
		{
			const auto& declared = dataType.dimension(level);
			if (declared.has_value())
				outerCounts.push_back(evaluateDimension(line, declared.value()));
			else
				hasInferredOuter = true;
		}

		u32 instanceCount = 1;
		if (!outerCounts.empty() || hasInferredOuter)
		{
			if (hasInferredOuter)
			{
				// Only the common single-level case is inferred (`u8[][Struct]`): deeper
				// inference would be ambiguous against the fields' own nesting.
				if (rank != 2 || !outerCounts.empty())
					error(line, "Only the outermost dimension may be inferred for a struct initialiser: {}", dataType.toString());
				if (value.empty())
					error(line, "Cannot work out dimension 0 of {}: it is empty", dataType.toString());
				instanceCount = static_cast<u32>(value.elements().size());
				outerCounts.push_back(instanceCount);
			}
			else
			{
				for (u32 count : outerCounts)
					instanceCount *= count;
			}
		}

		std::vector<LiteralScalar> outBytes;
		outBytes.reserve(static_cast<usize>(instanceCount) * layout.totalSize);

		if (outerCounts.empty())
		{
			resolveStructInstance(line, layout, value.elements(), outBytes);
		}
		else
		{
			if (value.elements().size() != instanceCount)
				error(line, "The initialiser has {} element(s) but {} declares {} struct instance(s)",
					value.elements().size(), dataType.toString(), instanceCount);
			for (const auto& element : value.elements())
			{
				if (!element.isGroup())
					error(line, "Expected a nested initialiser [...] for each '{}' instance", layout.name);
				resolveStructInstance(line, layout, element.group(), outBytes);
			}
		}

		std::vector<u32> byteDims = std::move(outerCounts);
		byteDims.push_back(layout.totalSize);
		DataType byteType = DataType::makeArray(DataTypeScalarCode::U8, byteDims).withAlias(dataType.alias());
		return std::pair{ byteType, LiteralValue::make(std::move(outBytes)) };
	}

	void TranslationUnitBuilder::resolveStructInstance(u32 line, const StructLayout& layout,
		std::span<const LiteralValueReferenceElement> elements, std::vector<LiteralScalar>& outBytes) const
	{
		if (elements.size() > layout.fields.size())
			error(line, "Too many values for struct '{}': it has {} field(s) but {} were given",
				layout.name, layout.fields.size(), elements.size());

		u32 offset = 0;
		for (usize i = 0; i < layout.fields.size(); ++i)
		{
			const StructFieldLayout& field = layout.fields[i];
			// The offsets were worked out once, when the struct was declared, and are what
			// `Struct.field` resolves to. Padding up to them rather than re-deriving the alignment
			// rule here is what keeps the bytes and the constants from ever disagreeing.
			if (offset < field.offset)
			{
				outBytes.insert(outBytes.end(), field.offset - offset, LiteralScalar::makeU8(0));
				offset = field.offset;
			}

			if (i < elements.size())
				appendStructFieldBytes(line, layout, field, elements[i], outBytes);
			else
			{
				// A short initialiser zero-fills the remaining fields, like arrays do.
				const u32 fieldBytes = field.type.sizeInBytes().value_or(0);
				outBytes.insert(outBytes.end(), fieldBytes, LiteralScalar::makeU8(0));
			}
			offset += field.type.sizeInBytes().value_or(0);
		}

		while (offset < layout.totalSize)
		{
			outBytes.push_back(LiteralScalar::makeU8(0));
			++offset;
		}
	}

	void TranslationUnitBuilder::appendStructFieldBytes(u32 line, const StructLayout& layout,
		const StructFieldLayout& field, const LiteralValueReferenceElement& element,
		std::vector<LiteralScalar>& outBytes) const
	{
		// A field that is itself a struct, or an array of them. Its type resolved to plain u8 bytes
		// like any other, so without this the group would be read as those bytes.
		if (!field.structName.empty())
		{
			const auto nestedRef = _translationUnit.resolveStruct(field.structName);
			if (!nestedRef.has_value())
				error(line, "Field '{}.{}' names struct '{}', which is not visible here", layout.name, field.name, field.structName);
			const StructLayout& nested = nestedRef->get();

			if (!element.isGroup())
				error(line, "Field '{}.{}' is a '{}' and expects a nested initialiser [...]", layout.name, field.name, nested.name);

			const u32 fieldBytes = field.type.sizeInBytes().value_or(0);
			const u32 instances = nested.totalSize > 0 ? fieldBytes / nested.totalSize : 0;

			if (instances <= 1)
			{
				resolveStructInstance(line, nested, element.group(), outBytes);
				return;
			}

			if (element.group().size() != instances)
				error(line, "Field '{}.{}' is {} '{}' instance(s) but {} were given",
					layout.name, field.name, instances, nested.name, element.group().size());

			for (const auto& instance : element.group())
			{
				if (!instance.isGroup())
					error(line, "Expected a nested initialiser [...] for each '{}' in '{}.{}'", nested.name, layout.name, field.name);
				resolveStructInstance(line, nested, instance.group(), outBytes);
			}
			return;
		}

		if (field.type.isScalar())
		{
			if (element.isGroup())
				error(line, "Field '{}.{}' expects a single value, not a nested initialiser", layout.name, field.name);
			const LiteralScalar resolved = evaluateElement(line, element, field.type.scalarCode());
			checkAliasBounds(line, field.type.alias(), std::span<const LiteralScalar>(&resolved, 1));
			appendScalarBytes(resolved, outBytes);
			return;
		}

		if (!element.isGroup())
			error(line, "Field '{}.{}' expects a nested initialiser [...] of {} element(s)", layout.name, field.name, field.type.numElements());

		std::vector<u32> dims;
		dims.reserve(field.type.rank());
		for (u8 i = 0; i < field.type.rank(); ++i)
			dims.push_back(field.type.dimension(i));

		std::vector<LiteralScalar> resolved;
		flattenLiteral(line, element.group(), dims, field.type.scalarCode(), resolved);
		checkAliasBounds(line, field.type.alias(), resolved);
		for (const LiteralScalar& scalar : resolved)
			appendScalarBytes(scalar, outBytes);
	}

	void TranslationUnitBuilder::appendScalarBytes(LiteralScalar scalar, std::vector<LiteralScalar>& outBytes)
	{
		const u32 raw = scalar.rawBits();
		const u32 byteCount = LiteralScalar::bitWidthOf(scalar.scalarCode()) / 8;
		for (u32 i = 0; i < byteCount; ++i)
			outBytes.push_back(LiteralScalar::makeU8(static_cast<u8>((raw >> (8 * i)) & 0xFFu)));
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

	OptionalConstRef<TranslationUnit> TranslationUnit::moduleNamed(std::string_view alias) const
	{
		for (const auto& entry : _directImports)
		{
			if (entry.alias == alias)
				return state().getTranslationUnit(entry.path);
		}
		return std::nullopt;
	}

	OptionalConstRef<Symbol> TranslationUnit::resolveSymbol(std::string_view name) const
	{
		// A qualified name is answered by exactly one module, so it never has to be disambiguated.
		if (const auto qualified = splitQualifiedName(name); qualified.has_value())
		{
			if (auto module = moduleNamed(qualified->first); module.has_value())
			{
				if (auto found = module->get().symbolTable().get(qualified->second);
					found.has_value() && isExported(found->get()))
					return found;
				return std::nullopt;
			}
			// Not a module name: fall through, because a local label is stored as `parent.name`.
		}

		if (auto own = _symbolTable.get(name); own.has_value())
			return own;

		const auto imported = lookupImportedSymbol(name);
		if (!imported.has())
			return std::nullopt;

		return std::cref(*imported.found);
	}

	OptionalConstRef<Macro> TranslationUnit::resolveMacro(const MacroSignature& signature) const
	{
		if (const auto qualified = splitQualifiedName(signature.name); qualified.has_value())
		{
			if (auto module = moduleNamed(qualified->first); module.has_value())
			{
				const auto bare = MacroSignature::make(qualified->second, signature.parameterCount);
				if (auto found = module->get().macroTable().getMacro(bare);
					found.has_value() && isExported(found->get()))
					return found;
				return std::nullopt;
			}
		}

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

		for (const auto& entry : _directImports)
		{
			if (auto unit = state().getTranslationUnit(entry.path); unit.has_value())
				unit->get().collectExportedSymbol(name, visited, result);
		}

		return result;
	}

	TranslationUnit::ImportLookup<Macro> TranslationUnit::lookupImportedMacro(const MacroSignature& signature) const
	{
		ImportLookup<Macro> result;
		std::vector<const TranslationUnit*> visited;
		visited.push_back(this);

		for (const auto& entry : _directImports)
		{
			if (auto unit = state().getTranslationUnit(entry.path); unit.has_value())
				unit->get().collectExportedMacro(signature, visited, result);
		}

		return result;
	}

	std::string_view TranslationUnit::findUnexportedSymbolOrigin(std::string_view name) const
	{
		std::string_view origin;
		std::vector<const TranslationUnit*> visited;
		visited.push_back(this);

		for (const auto& entry : _directImports)
		{
			if (auto unit = state().getTranslationUnit(entry.path); unit.has_value())
				unit->get().collectUnexportedSymbol(name, visited, origin);
		}

		return origin;
	}

	std::string_view TranslationUnit::findUnexportedMacroOrigin(const MacroSignature& signature) const
	{
		std::string_view origin;
		std::vector<const TranslationUnit*> visited;
		visited.push_back(this);

		for (const auto& entry : _directImports)
		{
			if (auto unit = state().getTranslationUnit(entry.path); unit.has_value())
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

		for (const auto& entry : _directImports)
		{
			if (auto unit = state().getTranslationUnit(entry.path); unit.has_value())
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

		for (const auto& entry : _directImports)
		{
			if (auto unit = state().getTranslationUnit(entry.path); unit.has_value())
				unit->get().collectUnexportedMacro(signature, visited, origin);
		}
	}

	void TranslationUnit::defineStruct(StructLayout&& layout)
	{
		auto key = layout.name;
		if (_structTable.contains(key))
			throw AssemblerError(_file, 0, 1, std::format("Struct '{}' is already defined", key));
		_structTable.emplace(std::move(key), std::move(layout));
	}

	OptionalConstRef<StructLayout> TranslationUnit::getStruct(std::string_view name) const
	{
		if (auto it = _structTable.find(std::string(name)); it != _structTable.end())
			return std::cref(it->second);
		return std::nullopt;
	}

	OptionalConstRef<StructLayout> TranslationUnit::resolveStruct(std::string_view name) const
	{
		// A qualified name is answered by exactly one module, like symbols.
		if (const auto qualified = splitQualifiedName(name); qualified.has_value())
		{
			if (auto module = moduleNamed(qualified->first); module.has_value())
			{
				ImportLookup<StructLayout> result;
				std::vector<const TranslationUnit*> visited;
				visited.push_back(this);
				module->get().collectExportedStruct(qualified->second, visited, result);
				// Own table first so a struct shadowed deeper in the graph still resolves.
				if (auto own = module->get().getStruct(qualified->second);
					own.has_value() && own->get().isGlobal)
					return own;
				if (result.has())
					return std::cref(*result.found);
				return std::nullopt;
			}
			// Not a module name: fall through, because a field offset is stored as `parent.name`.
		}

		if (auto own = getStruct(name); own.has_value())
			return own;

		ImportLookup<StructLayout> result;
		std::vector<const TranslationUnit*> visited;
		visited.push_back(this);

		for (const auto& entry : _directImports)
		{
			if (auto unit = state().getTranslationUnit(entry.path); unit.has_value())
				unit->get().collectExportedStruct(name, visited, result);
		}

		if (!result.has())
			return std::nullopt;

		return std::cref(*result.found);
	}

	void TranslationUnit::collectExportedStruct(std::string_view name, std::vector<const TranslationUnit*>& visited, ImportLookup<StructLayout>& result) const
	{
		if (std::find(visited.begin(), visited.end(), this) != visited.end())
			return;
		visited.push_back(this);

		if (auto own = getStruct(name); own.has_value() && own->get().isGlobal)
		{
			const StructLayout* layout = &own->get();
			if (result.found == nullptr)
			{
				result.found = layout;
				result.foundIn = _file;
			}
			else if (result.found != layout && !result.ambiguous)
			{
				result.ambiguous = true;
				result.alsoIn = _file;
			}
		}

		for (const auto& entry : _directImports)
		{
			if (auto unit = state().getTranslationUnit(entry.path); unit.has_value())
				unit->get().collectExportedStruct(name, visited, result);
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

		for (const auto& entry : _directImports)
		{
			if (auto unit = state().getTranslationUnit(entry.path); unit.has_value())
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

		for (const auto& entry : _directImports)
		{
			if (auto unit = state().getTranslationUnit(entry.path); unit.has_value())
				unit->get().collectExportedMacro(signature, visited, result);
		}
	}
}

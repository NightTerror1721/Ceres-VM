#include "translation_unit.h"
#include "instruction_info.h"
#include "assembly_state.h"

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

	DataType TranslationUnitBuilder::resolveDataType(u32 line, const DataTypeReference& dataType, bool allowUnsizedArrays) const
	{
		if (!dataType.isValid())
			error(line, "Invalid data type");

		if (!dataType.hasNumElementsIdentifier())
		{
			DataType resolvedDataType = dataType.isScalar()
				? DataType::makeScalar(dataType.scalarCode())
				: DataType::makeSizedArray(dataType.scalarCode(), dataType.numElementsIntegerValue());

			if (resolvedDataType.hasUnknownSize() && !allowUnsizedArrays)
				error(line, "Array data type without initial value must have a known size (either a specified size or an identifier for the size)");

			return resolvedDataType;
		}

		const auto constValue = getConstantValue(line, dataType.numElementsIdentifier());
		if (!constValue.has_value())
			error(line, "Invalid identifier for array size in data type");

		const auto& value = constValue.value().get();
		if (!value.isScalar() || !DataType::isIntegerScalarCode(value.scalarCode()))
			error(line, "Identifier for array size in data type must be a constant integer");

		u32 numElements = value.first().asRawValue();
		if (numElements == 0)
			error(line, "Array size in data type cannot be zero");

		return DataType::makeSizedArray(dataType.scalarCode(), numElements);
	}

	LiteralValue TranslationUnitBuilder::resolveLiteralValue(u32 line, const LiteralValueReference& value, bool allowEmptyArrays, std::optional<DataTypeScalarCode> targetScalarCode) const
	{
		if (value.empty())
		{
			if (!allowEmptyArrays)
				error(line, "Empty array literal value is not allowed");
			return LiteralValue::makeEmpty();
		}

		// Integer literals are untyped in source: `42` carries no width of its own. When the
		// declaration states one, every element is re-tagged to it here, and only here is the
		// value checked against the width it has to fit in.
		const auto narrow = [&](LiteralScalar scalar, usize index) -> LiteralScalar
		{
			if (!targetScalarCode.has_value() || scalar.scalarCode() == *targetScalarCode)
				return scalar;

			auto coerced = scalar.coerceTo(*targetScalarCode);
			if (!coerced.has_value())
			{
				if (!scalar.isInteger() || !DataType::isIntegerScalarCode(*targetScalarCode))
					error(line, "Element {} is of type {}, which cannot be converted to the declared type {}",
						index, DataType::scalarCodeToString(scalar.scalarCode()), DataType::scalarCodeToString(*targetScalarCode));

				error(line, "Element {} does not fit in the declared type {}: the value needs more than {} bits",
					index, DataType::scalarCodeToString(*targetScalarCode), LiteralScalar::bitWidthOf(*targetScalarCode));
			}
			return coerced.value();
		};

		std::vector<LiteralScalar> resolvedElements;
		resolvedElements.reserve(value.size());

		usize index = 0;
		for (const auto& elem : value.elements())
		{
			if (elem.isIdentifier())
			{
				auto constantValue = getConstantValue(line, elem.identifierValue());
				if (!constantValue.has_value())
					error(line, "Cannot resolve identifier literal value that is not a constant");
				const auto& resolvedValue = constantValue.value().get();
				if (!resolvedValue.isScalar())
					error(line, "Identifier literal array element value must resolve to a scalar constant");
				resolvedElements.push_back(narrow(resolvedValue.first(), index));
			}
			else if (elem.isScalar())
			{
				resolvedElements.push_back(narrow(elem.scalarValue(), index));
			}
			else
			{
				error(line, "Unknown literal value reference element type");
			}
			++index;
		}

		return LiteralValue::make(std::move(resolvedElements));
	}

	std::pair<DataType, LiteralValue> TranslationUnitBuilder::resolveLiteralValue(u32 line, const DataTypeReference& expectedDataType, const LiteralValueReference& value) const
	{
		DataType resolvedDataType = resolveDataType(line, expectedDataType, true);
		LiteralValue resolvedValue = resolveLiteralValue(line, value, !resolvedDataType.hasUnknownSize(), resolvedDataType.scalarCode());
		if (!resolvedValue.matchDataType(resolvedDataType))
			error(line, "Resolved literal value does not match the expected data type");

		return { resolvedDataType, std::move(resolvedValue) };
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

	std::optional<std::reference_wrapper<const LiteralValue>> TranslationUnitBuilder::getConstantValue(u32 line, std::string_view name) const noexcept
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

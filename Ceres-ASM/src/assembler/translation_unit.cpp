#include "translation_unit.h"
#include "instruction_info.h"

namespace ceres::casm
{
	void TranslationUnitBuilder::build(std::vector<Statement>&& statements)
	{
		if (_built)
			throw std::logic_error("Translation unit has already been built");

		_built = true;

		AssemblerErrorHandler& errorHandler = _translationUnit.errorHandler();
		SymbolTable& symbolTable = _translationUnit.symbolTable();
		std::vector<RelocatableStatement> ast;
		std::vector<std::string> unresolvedSymbols;
		const usize statementCount = statements.size();

		ast.reserve(statementCount);

		for (auto it = statements.begin(); it != statements.end(); ++it)
		{
			try
			{
				Statement& statement = *it;
				if (statement.isSection())
				{
					_currentSection = statement.asSection().section;
					ast.push_back(RelocatableStatement::makeSection(statement.line(), statement.asSection()));
				}
				else if (statement.isLabel())
				{
					if (!_currentSection.has_value())
						error(statement.line(), "Label statement must be preceded by a section statement");

					const auto& label = statement.asLabel();
					symbolTable.defineLabel(statement.line(), label.name, _currentSection.value(), currentOffset(), label.level);
					ast.push_back(RelocatableStatement::makeLabel(statement.line(), currentOffset(), label));
				}
				else if (statement.isData())
				{
					const auto& data = statement.asData();
					if (!_currentSection.has_value())
					{
						if (!data.isConstant)
							error(statement.line(), "Data statement must be preceded by a section statement");
					}
					
					DataType dataType = DataType::Invalid;
					std::optional<LiteralValue> literalValue = std::nullopt;
					std::expected<u32, std::string_view> size = 0;
					if (data.value.has_value())
					{
						if (data.dataType.has_value())
						{
							auto result = resolveLiteralValue(statement.line(), data.dataType.value(), data.value.value());
							dataType = result.first;
							literalValue = std::move(result.second);
							size = sizeOf(statement.line(), dataType, literalValue.value());
						}
						else
						{
							literalValue = resolveLiteralValue(statement.line(), data.value.value(), false);
							dataType = literalValue.value().dataType();
							size = sizeOf(statement.line(), literalValue.value());
						}
					}
					else if (data.dataType.has_value())
					{
						dataType = resolveDataType(statement.line(), data.dataType.value());
						size = sizeOf(statement.line(), dataType);
					}

					if (!dataType.isValid())
						error(statement.line(), "Failed to determine data type of data statement");

					if (literalValue.has_value() && !literalValue->matchDataType(dataType))
						error(statement.line(), "Literal value does not match the specified data type");

					if (!size.has_value() || size.value() == 0)
						error(statement.line(), "Failed to determine size of data statement: " + std::string(size.error()));

					if (data.isConstant)
					{
						if (!literalValue.has_value())
							error(statement.line(), "Constant data statement must have an initial value");

						if (_currentSection.has_value() && _currentSection.value() == SectionType::Rodata)
						{
							if (!data.dataType.has_value())
								error(statement.line(), "Constant data statement in @rodata section must have a data type");
							symbolTable.defineVariable(statement.line(), data.identifier, _currentSection.value(), currentOffset(), false, true, dataType, literalValue.value());
						}
						else
							symbolTable.defineConstant(statement.line(), data.identifier, false, literalValue.value());
					}
					else
					{
						if (!_currentSection.has_value())
							error(statement.line(), "Variable data statement must be preceded by a section statement");

						switch (_currentSection.value())
						{
							case SectionType::Text:
								error(statement.line(), "Variable data statement cannot be in the @text section");

							case SectionType::Rodata:
								error(statement.line(), "Variable data statement cannot be in the @rodata section");

							case SectionType::Data:
								if (literalValue.has_value())
									symbolTable.defineVariable(statement.line(), data.identifier, _currentSection.value(), currentOffset(), false, false, dataType, literalValue.value());
								else
									symbolTable.defineVariable(statement.line(), data.identifier, _currentSection.value(), currentOffset(), false, false, dataType);
								break;

							case SectionType::BSS:
								if (literalValue.has_value())
									error(statement.line(), "Variable data statement in @bss section cannot have an initial value");

								symbolTable.defineVariable(statement.line(), data.identifier, _currentSection.value(), currentOffset(), false, false, dataType);
								break;
						}
					}

					ast.push_back(RelocatableStatement::makeData(statement.line(), size.value(), currentOffset(), ResolvedDataStatement{data.isConstant, data.identifier, dataType, literalValue}));
					currentOffset() += size.value();
				}
				else if (statement.isInstruction())
				{
					if (!_currentSection.has_value() || _currentSection.value() != SectionType::Text)
						error(statement.line(), "Instruction statement must be in the @text section");

					auto& instruction = statement.asInstruction();
					for (auto& operand : instruction.operands)
						symbolTable.tryResolveOperand(statement.line(), operand, unresolvedSymbols);

					ast.push_back(RelocatableStatement::makeInstruction(statement.line(), currentOffset(), instruction));

					auto sizeOpt = InstructionInfo::findMaxSizeInBytes(instruction.mnemonic);
					if (!sizeOpt.has_value() || sizeOpt.value() == 0)
						error(statement.line(), "Failed to determine size of instruction statement");

					currentOffset() += sizeOpt.value();
				}
				else
				{
					error(statement.line(), "Unknown statement type");
				}
			}
			catch (const AssemblerError& error)
			{
				errorHandler.reportError(error);
				it = statements.erase(it);
			}
		}

		_translationUnit.setAST(std::move(ast));
		_translationUnit.setUnresolvedSymbols(std::move(unresolvedSymbols));
	}

	DataType TranslationUnitBuilder::resolveDataType(u32 line, const DataTypeReference& dataType) const
	{
		if (!dataType.isValid())
			error(line, "Invalid data type");

		if (!dataType.hasNumElementsIdentifier())
		{
			DataType resolvedDataType = dataType.isScalar()
				? DataType::makeScalar(dataType.scalarCode())
				: DataType::makeSizedArray(dataType.scalarCode(), dataType.numElementsIntegerValue());

			if (resolvedDataType.hasUnknownSize())
				error(line, "Array data type without initial value must have a known size (either a specified size or an identifier for the size)");

			return resolvedDataType;
		}

		const auto constValue = getConstantValue(line, dataType.numElementsIdentifier().name());
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

	LiteralValue TranslationUnitBuilder::resolveLiteralValue(u32 line, const LiteralValueReference& value, bool allowEmptyArrays) const
	{
		if (value.empty())
		{
			if (!allowEmptyArrays)
				error(line, "Empty array literal value is not allowed");
			return LiteralValue::makeEmpty();
		}

		std::vector<LiteralScalar> resolvedElements;
		resolvedElements.reserve(value.size());

		for (const auto& elem : value.elements())
		{
			if (elem.isIdentifier())
			{
				auto constantValue = getConstantValue(line, elem.identifierValue().name());
				if (!constantValue.has_value())
					error(line, "Cannot resolve identifier literal value that is not a constant");
				const auto& resolvedValue = constantValue.value().get();
				if (!resolvedValue.isScalar())
					error(line, "Identifier literal array element value must resolve to a scalar constant");
				resolvedElements.push_back(resolvedValue.first());
			}
			else if (elem.isScalar())
			{
				resolvedElements.push_back(elem.scalarValue());
			}
			else
			{
				error(line, "Unknown literal value reference element type");
			}
		}

		return LiteralValue::make(std::move(resolvedElements));
	}

	std::pair<DataType, LiteralValue> TranslationUnitBuilder::resolveLiteralValue(u32 line, const DataTypeReference& expectedDataType, const LiteralValueReference& value) const
	{
		DataType resolvedDataType = resolveDataType(line, expectedDataType);
		LiteralValue resolvedValue = resolveLiteralValue(line, value, true);
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

		return sizeOf(line, dataType);
	}

	std::optional<std::reference_wrapper<const LiteralValue>> TranslationUnitBuilder::getConstantValue(u32 line, std::string_view name) const noexcept
	{
		if (auto result = _translationUnit.symbolTable().get(name); result.has_value())
		{
			const Symbol& symbol = result.value().get();
			if (symbol.isConstant())
				return std::cref(symbol.value());
		}
		return std::nullopt;
	}
}

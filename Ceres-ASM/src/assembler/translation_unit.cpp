#include "translation_unit.h"
#include "vm/instructions.h"

namespace ceres::casm
{
	void TranslationUnitBuilder::build(std::vector<Statement>&& statements)
	{
		if (_built)
			throw std::logic_error("Translation unit has already been built");

		_built = true;

		AssemblerErrorHandler& errorHandler = _translationUnit.errorHandler();
		SymbolTable& symbolTable = _translationUnit.symbolTable();
		std::vector<std::string> unresolvedSymbols;
		const usize statementCount = statements.size();
		for (auto it = statements.begin(); it != statements.end(); ++it)
		{
			try
			{
				Statement& statement = *it;
				if (statement.isSection())
				{
					_currentSection = statement.asSection().section;
				}
				else if (statement.isLabel())
				{
					if (!_currentSection.has_value())
						error(statement.line(), "Label statement must be preceded by a section statement");

					const auto& label = statement.asLabel();
					symbolTable.defineLabel(statement.line(), label.name, currentOffset(), label.level);
				}
				else if (statement.isData())
				{
					auto& data = statement.asData();
					if (!_currentSection.has_value())
					{
						if (!data.isConstant)
							error(statement.line(), "Data statement must be preceded by a section statement");
					}
					
					u32 size = 0;
					if (data.value.has_value())
					{
						if (data.dataType.has_value())
						{
							resolveLiteralValue(statement.line(), data.dataType.value(), data.value.value());
							size = sizeOf(statement.line(), data.dataType.value(), data.value.value());
						}
						else
						{
							resolveLiteralValue(statement.line(), data.value.value(), false);
							size = sizeOf(statement.line(), data.value.value());
						}
					}
					else if (data.dataType.has_value())
					{
						resolveDataType(statement.line(), data.dataType.value());
						size = sizeOf(statement.line(), data.dataType.value());
					}

					if (size == 0)
						error(statement.line(), "Data statement must have a non-zero size");

					if (data.isConstant)
					{
						if (!data.value.has_value())
							error(statement.line(), "Constant data statement must have an initial value");

						if (_currentSection.has_value() && _currentSection.value() == SectionType::Rodata)
						{
							if (!data.dataType.has_value())
								error(statement.line(), "Constant data statement in @rodata section must have a data type");
							symbolTable.defineVariable(statement.line(), data.identifier, currentOffset(), false, true, data.dataType.value(), data.value.value());
						}
						else
							symbolTable.defineConstant(statement.line(), data.identifier, false, data.value.value());
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
								if (!data.dataType.has_value())
									error(statement.line(), "Variable data statement must have a data type");

								if (data.value.has_value())
									symbolTable.defineVariable(statement.line(), data.identifier, currentOffset(), false, false, data.dataType.value(), data.value.value());
								else
									symbolTable.defineVariable(statement.line(), data.identifier, currentOffset(), false, false, data.dataType.value());
								break;

							case SectionType::BSS:
								if (!data.dataType.has_value())
									error(statement.line(), "Variable data statement must have a data type");
								if (data.value.has_value())
									error(statement.line(), "Variable data statement in @bss section cannot have an initial value");

								symbolTable.defineVariable(statement.line(), data.identifier, currentOffset(), false, false, data.dataType.value());
								break;
						}
					}

					statement.setAddress(currentOffset());
					currentOffset() += size;
				}
				else if (statement.isInstruction())
				{
					if (!_currentSection.has_value() || _currentSection.value() != SectionType::Text)
						error(statement.line(), "Instruction statement must be in the @text section");

					auto& instruction = statement.asInstruction();
					for (auto& operand : instruction.operands)
						symbolTable.tryResolveOperand(statement.line(), operand, unresolvedSymbols);

					statement.setAddress(currentOffset());
					currentOffset() += vm::Instruction::SizeInBytes;
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

		_translationUnit.setAST(std::move(statements));
		_translationUnit.setUnresolvedSymbols(std::move(unresolvedSymbols));
	}

	void TranslationUnitBuilder::resolveDataType(u32 line, DataTypeInfo& dataType) const
	{
		switch (dataType.category())
		{
			case DataTypeCategory::Scalar:
				if (!isScalarDataType(dataType.type()))
					error(line, "Invalid scalar data type");
				return;

			case DataTypeCategory::UnsizedArray:
				if (dataType.type() == DataType::String)
					return; // String is a valid unsized array data type
				if (!isScalarDataType(dataType.type()))
					error(line, "Invalid unsized array data type");
				if (dataType.hasArraySize())
					error(line, "Unsized array data type should not have an array size");
				return;

			case DataTypeCategory::SizedArray:
				if (!isScalarDataType(dataType.type()))
					error(line, "Invalid sized array data type");
				if (!dataType.hasArraySize())
					error(line, "Sized array data type should have an array size");
				if (dataType.hasIdentifierArraySize())
				{
					const auto constValue = getConstantValue(line, dataType.identifierArraySize().name());
					if (!constValue.has_value() || !constValue.value().get().isInteger())
						error(line, "Invalid identifier array size for sized array data type");

					u32 size = constValue.value().get().integerValue();
					if (size == 0)
						error(line, "Invalid identifier array size of 0 for sized array data type");

					dataType.setArraySize(size);
				}
				return;
		}
	}

	void TranslationUnitBuilder::resolveLiteralValue(u32 line, LiteralValue& value, bool allowEmptyArrays) const
	{
		switch (value.type())
		{
			case LiteralValueType::Integer:
			case LiteralValueType::Float:
			case LiteralValueType::Char:
			case LiteralValueType::Bool:
			case LiteralValueType::String:
				return; // No resolution needed for these types

			case LiteralValueType::Array:
			{
				auto& array = value.arrayValueMutable();
				if (array.empty() && !allowEmptyArrays)
					error(line, "Empty array literal value is not allowed");
				for (auto& elem : array)
					resolveLiteralValue(line, elem); // Recursively resolve each element
				const auto& elementType = array.begin()->type();
				for (const auto& elem : array)
				{
					if (elem.type() != elementType)
						error(line, "Array literal value contains elements of different types");
				}
				return;
			}

			case LiteralValueType::Identifier:
			{
				auto constantValue = getConstantValue(line, value.identifierValue().name());
				if (!constantValue.has_value())
					error(line, "Cannot resolve identifier literal value that is not a constant");
				value.setValue(constantValue.value().get()); // Replace identifier with its constant value
				return;
			}
		}
	}

	void TranslationUnitBuilder::resolveLiteralValue(u32 line, DataTypeInfo& expectedDataType, LiteralValue& value) const
	{
		resolveDataType(line, expectedDataType);
		resolveLiteralValue(line, value, true);
		if (!expectedDataType.matchLiteralValue(value, true))
			error(line, "Literal value does not match the expected data type");
	}

	u32 TranslationUnitBuilder::sizeOf(u32 line, const DataTypeInfo& dataType) const
	{
		u32 elementSize = 0;
		switch (dataType.type())
		{
			case DataType::U8:
			case DataType::I8:
			case DataType::Char:
			case DataType::Bool:
			case DataType::String:
				elementSize = 1;
				break;

			case DataType::U16:
			case DataType::I16:
				elementSize = 2;
				break;

			case DataType::U32:
			case DataType::I32:
				elementSize = 4;
				break;
		}

		switch (dataType.category())
		{
			case DataTypeCategory::Scalar:
				return elementSize;

			case DataTypeCategory::UnsizedArray:
				error(line, "Cannot determine the size of an unsized array data type");

			case DataTypeCategory::SizedArray:
				if (dataType.hasIntegerArraySize())
					return elementSize * dataType.integerArraySize();
				error(line, "Cannot determine the size of a sized array data type with an identifier array size");
		}

		std::unreachable();
	}

	u32 TranslationUnitBuilder::sizeOf(u32 line, const LiteralValue& value) const
	{
		switch (value.type())
		{
			case LiteralValueType::Integer:
				return 4; // Assuming u32 for integer literals

			case LiteralValueType::Float:
				return 4; // Assuming f32 for float literals

			case LiteralValueType::Char:
				return 1; // Assuming char is 1 byte

			case LiteralValueType::Bool:
				return 1; // Assuming bool is 1 byte

			case LiteralValueType::String:
				return static_cast<u32>(value.stringValue().size() + 1); // +1 for null terminator

			case LiteralValueType::Array:
			{
				const auto& array = value.arrayValue();
				u32 totalSize = 0;
				for (const auto& elem : array)
					totalSize += sizeOf(line, elem);
				return totalSize;
			}

			case LiteralValueType::Identifier:
			{
				auto constantValue = getConstantValue(line, value.identifierValue().name());
				if (!constantValue.has_value())
					error(line, "Cannot determine the size of an identifier literal value that is not a constant");
				return sizeOf(line, constantValue.value().get());
			}
		}

		std::unreachable();
	}

	u32 TranslationUnitBuilder::sizeOf(u32 line, const DataTypeInfo& dataType, const LiteralValue& value) const
	{
		if (!dataType.matchLiteralValue(value, true))
			error(line, "Data type does not match the literal value");

		u32 elementSize = 0;
		switch (dataType.type())
		{
			case DataType::U8:
			case DataType::I8:
			case DataType::Char:
			case DataType::Bool:
			case DataType::String:
				elementSize = 1;
				break;

			case DataType::U16:
			case DataType::I16:
				elementSize = 2;
				break;

			case DataType::U32:
			case DataType::I32:
				elementSize = 4;
				break;
		}

		switch (dataType.category())
		{
			case DataTypeCategory::Scalar:
				return elementSize;

			case DataTypeCategory::UnsizedArray:
			{
				if (value.isString())
				{
					if (dataType.type() != DataType::String)
						error(line, "Cannot determine the size of an unsized array data type with a string literal value that is not a string data type");
					return static_cast<u32>(value.stringValue().size() + 1); // +1 for null terminator
				}

				if (!value.isArray())
				{
					error(line,
						"Cannot determine the size of an unsized array data type with a non-array literal value or an array literal value with incompatible element types"
					);
				}

				const auto& array = value.arrayValue();
				if (array.empty())
					error(line, "Cannot determine the size of an unsized array data type with an empty array literal value");

				return elementSize * static_cast<u32>(array.size());
			}

			case DataTypeCategory::SizedArray:
			{
				if (dataType.type() == DataType::String)
					error(line, "Cannot determine the size of a sized array data type with a string literal value");
				if (!value.isArray())
					error(line, "Cannot determine the size of a sized array data type with a non-array literal value");
				if (!dataType.hasIntegerArraySize())
					error(line, "Cannot determine the size of a sized array data type with an identifier array size");

				const auto& array = value.arrayValue();
				if (array.size() != dataType.integerArraySize())
					error(line, "Cannot determine the size of a sized array data type with an array literal value of a different size");
				if (dataType.integerArraySize() == 0)
					error(line, "Cannot determine the size of a sized array data type with an array literal value of size 0");

				return elementSize * dataType.integerArraySize();
			}
		}

		std::unreachable();
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

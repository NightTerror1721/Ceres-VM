#include "binary_emitter.h"

namespace ceres::casm
{
	std::optional<vm::Program> BinaryEmitter::emit()
	{
		Address entryPoint = Address::Null;
		auto mainSymbol = _linkedExecutable.globalSymbolTable().get(SymbolTable::EntryPointLabelName);
		if (!mainSymbol.has_value() || !mainSymbol.value().get().isLabel() || !mainSymbol.value().get().isGlobal() || !mainSymbol.value().get().hasAddress())
		{
			reportError(0, "Entry point label '{}' is not defined or not a global label", SymbolTable::EntryPointLabelName);
			return std::nullopt;
		}
		entryPoint = mainSymbol.value().get().address();

		for (const auto& unit : _linkedExecutable.units())
		{
			std::optional<SectionType> currentSection = std::nullopt;
			for (const auto& statement : unit.ast())
			{
				if (statement.isSection())
				{
					currentSection = statement.asSection().section;
				}
				else if (statement.isData())
				{
					if (!currentSection.has_value())
					{
						reportError(statement.line(), "Data statement must be preceded by a section statement");
						return std::nullopt;
					}
					switch (currentSection.value())
					{
						case SectionType::Data:
							emitData(statement, false);
							break;

						case SectionType::Rodata:
							emitData(statement, true);
							break;

						case SectionType::BSS:
							// BSS section does not have initial values, so we don't emit data for it.
							break;

						case SectionType::Text:
							reportError(statement.line(), "Data statement cannot be in the text section");
							return std::nullopt;

						default:
							reportError(statement.line(), "Unsupported section type");
							return std::nullopt;
					}
					
				}
				else if (statement.isInstruction())
				{
					if (!currentSection.has_value() || currentSection.value() != SectionType::Text)
					{
						reportError(statement.line(), "Instruction statement must be in the text section");
						return std::nullopt;
					}
					emitInstruction(statement);
				}
			}
		}

		vm::ProgramHeader header{
			.magic = vm::ProgramHeader::MagicNumber,
			.version = vm::ProgramHeader::CurrentVersion,
			.entryPoint = entryPoint.value(),
			.textSize = _linkedExecutable.memoryMap().textSize,
			.rodataSize = _linkedExecutable.memoryMap().rodataSize,
			.dataSize = _linkedExecutable.memoryMap().dataSize,
			.bssSize = _linkedExecutable.memoryMap().bssSize,
			.minimumStack = 1024 // For now, we can set this to 1024. In the future, we might want to calculate the minimum stack size based on the program's requirements.
		};

		return vm::Program::make(
			header,
			std::move(_textBuffer),
			std::move(_rodataBuffer),
			std::move(_dataBuffer)
		);
	}

	void BinaryEmitter::emitData(const RelocatableStatement& statement, bool isRodata)
	{
		const ResolvedDataStatement& data = statement.asData();
		std::vector<u8>& buffer = isRodata ? _rodataBuffer : _dataBuffer;
		
		if (data.value.has_value())
		{
			const LiteralValue& value = data.value.value();
			DataType type = data.dataType;
			if (value.hasUnknownSize())
			{
				reportError(statement.line(), "Data statement has an initial value with unknown size");
				return;
			}

			u32 size = type.sizeInBytes().value_or(0);
			if (size == 0)
			{
				reportError(statement.line(), "Cannot determine size of data statement");
				return;
			}

			for (const auto& scalarValue : value.elements())
			{
				switch (type.scalarCode())
				{
					case DataTypeScalarCode::U8:
						writeToBuffer(buffer, static_cast<u8>(scalarValue.value().u8Value));
						break;

					case DataTypeScalarCode::U16:
						writeToBuffer(buffer, static_cast<u16>(scalarValue.value().u16Value));
						break;

					case DataTypeScalarCode::U32:
						writeToBuffer(buffer, static_cast<u32>(scalarValue.value().u32Value));
						break;

					case DataTypeScalarCode::I8:
						writeToBuffer(buffer, static_cast<i8>(scalarValue.value().i8Value));
						break;

					case DataTypeScalarCode::I16:
						writeToBuffer(buffer, static_cast<i16>(scalarValue.value().i16Value));
						break;

					case DataTypeScalarCode::I32:
						writeToBuffer(buffer, static_cast<i32>(scalarValue.value().i32Value));
						break;

					case DataTypeScalarCode::F32:
						writeToBuffer(buffer, static_cast<f32>(scalarValue.value().f32Value));
						break;

					default:
						reportError(statement.line(), "Unsupported data type for scalar value in data statement");
						return;
				}
			}
		}
		else
		{
			if (isRodata)
			{
				reportError(0, "Read-only data statement must have an initial value");
				return;
			}

			u32 size = statement.size();
			if (size == 0)
			{
				reportError(statement.line(), "Data statement must have a non-zero size");
				return;
			}

			buffer.resize(buffer.size() + size, 0);
		}
	}

	void BinaryEmitter::emitInstruction(const RelocatableStatement& statement)
	{
		const InstructionStatement& instruction = statement.asInstruction();
		auto infoOpt = InstructionInfo::find(instruction.signature());
		if (!infoOpt.has_value())
		{
			reportError(statement.line(), "Unknown instruction signature: {}", instruction.signature().toString());
			return;
		}

		isize remainingOpcodes = infoOpt.value().maxOpcodeCountAtAllOverloads();

		const InstructionInfo& info = infoOpt.value();
		for (const auto& opcodeInfo : info.opcodes())
		{
			vm::Instruction encodedInstruction;
			encodedInstruction.setOpcode(opcodeInfo.opcode());

			for (usize i = 0; i < OpcodeInfo::MaxParametersPerOpcode; i++)
			{
				const auto& param = opcodeInfo.parameterAt(i);
				if (param.isInvalid())
					break;

				if (param.isFixed())
				{
					switch (param.type())
					{
						case OpcodeParameterType::RD:
							encodedInstruction.setRd(param.fixedValueU8());
							break;

						case OpcodeParameterType::RS:
							encodedInstruction.setRs(param.fixedValueU8());
							break;

						case OpcodeParameterType::RT:
							encodedInstruction.setRt(param.fixedValueU8());
							break;

						case OpcodeParameterType::FD:
							encodedInstruction.setFd(param.fixedValueU8());
							break;

						case OpcodeParameterType::FS:
							encodedInstruction.setFs(param.fixedValueU8());
							break;

						case OpcodeParameterType::FT:
							encodedInstruction.setFt(param.fixedValueU8());
							break;

						case OpcodeParameterType::IMM8:
							encodedInstruction.setImm8(param.fixedValueU8());
							break;

						case OpcodeParameterType::IMM16:
							encodedInstruction.setImm16(param.fixedValueU16());
							break;

						case OpcodeParameterType::SIMM16:
							encodedInstruction.setSImm16(param.fixedValueS16());
							break;

						case OpcodeParameterType::IMM24:
							encodedInstruction.setImm24(param.fixedValueU24());
							break;

						case OpcodeParameterType::SIMM24:
							encodedInstruction.setSImm24(param.fixedValueS24());
							break;

						default:
							reportError(statement.line(), "Unsupported fixed parameter type for instruction encoding");
							return;
					}
				}
				else
				{
					u8 operandIndex = param.operandIndex();
					if (operandIndex >= instruction.operands.size())
					{
						reportError(statement.line(), "Operand index {} out of range for instruction encoding", operandIndex);
						return;
					}

					const auto& operandInfo = instruction.operands[operandIndex];
					switch (param.type())
					{
						case OpcodeParameterType::RD:
							encodedInstruction.setRd(operandInfo.asRegister().regIndex);
							break;

						case OpcodeParameterType::RS:
							encodedInstruction.setRs(operandInfo.asRegister().regIndex);
							break;

						case OpcodeParameterType::RT:
							encodedInstruction.setRt(operandInfo.asRegister().regIndex);
							break;

						case OpcodeParameterType::FD:
							encodedInstruction.setFd(operandInfo.asFloatingPointRegister().regIndex);
							break;

						case OpcodeParameterType::FS:
							encodedInstruction.setFs(operandInfo.asFloatingPointRegister().regIndex);
							break;

						case OpcodeParameterType::FT:
							encodedInstruction.setFt(operandInfo.asFloatingPointRegister().regIndex);
							break;

						case OpcodeParameterType::IMM8:
							encodedInstruction.setImm8(static_cast<u8>(operandInfo.asImmediate().value << param.fixedValueShift()));
							break;

						case OpcodeParameterType::IMM16:
							encodedInstruction.setImm16(static_cast<u16>(operandInfo.asImmediate().value << param.fixedValueShift()));
							break;

						case OpcodeParameterType::SIMM16:
							encodedInstruction.setSImm16(static_cast<i16>(operandInfo.asImmediate().value << param.fixedValueShift()));
							break;

						case OpcodeParameterType::IMM24:
							encodedInstruction.setImm24(static_cast<u24>(operandInfo.asImmediate().value << param.fixedValueShift()));
							break;

						case OpcodeParameterType::SIMM24:
							encodedInstruction.setSImm24(static_cast<i24>(operandInfo.asImmediate().value << param.fixedValueShift()));
							break;

						case OpcodeParameterType::RD_IMM16:
							encodedInstruction.setRd(operandInfo.asMemory().baseRegIndex);
							encodedInstruction.setImm16(static_cast<u16>(instruction.operands[operandIndex + 1].asMemory().immediateOffset().value));
							break;

						case OpcodeParameterType::RS_IMM16:
							encodedInstruction.setRs(operandInfo.asMemory().baseRegIndex);
							encodedInstruction.setImm16(static_cast<u16>(instruction.operands[operandIndex + 1].asMemory().immediateOffset().value));
							break;

						case OpcodeParameterType::RT_IMM16:
							encodedInstruction.setRt(operandInfo.asMemory().baseRegIndex);
							encodedInstruction.setImm16(static_cast<u16>(instruction.operands[operandIndex + 1].asMemory().immediateOffset().value));
							break;

						default:
							reportError(statement.line(), "Unsupported operand type for instruction encoding");
							return;
					}
				}
			}

			if (remainingOpcodes <= 0)
			{
				reportError(statement.line(), "No matching opcode found for instruction signature: {}", instruction.signature().toString());
				return;
			}

			remainingOpcodes--;
			writeToBuffer(_textBuffer, encodedInstruction);
		}

		while (remainingOpcodes > 0)
		{
			writeToBuffer(_textBuffer, vm::Instruction::NOP());
			remainingOpcodes--;
		}
	}
}
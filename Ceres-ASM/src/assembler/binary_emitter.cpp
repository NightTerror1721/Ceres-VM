#include "binary_emitter.h"
#include <optional>

namespace ceres::casm
{
	namespace
	{
		// The two enumerations happen to agree today, but debug::ScalarType is a file format and
		// DataTypeScalarCode is an implementation detail, so they are translated rather than cast.
		u8 toDebugScalarType(DataTypeScalarCode code) noexcept
		{
			switch (code)
			{
				case DataTypeScalarCode::U8:  return static_cast<u8>(debug::ScalarType::U8);
				case DataTypeScalarCode::U16: return static_cast<u8>(debug::ScalarType::U16);
				case DataTypeScalarCode::U32: return static_cast<u8>(debug::ScalarType::U32);
				case DataTypeScalarCode::I8:  return static_cast<u8>(debug::ScalarType::I8);
				case DataTypeScalarCode::I16: return static_cast<u8>(debug::ScalarType::I16);
				case DataTypeScalarCode::I32: return static_cast<u8>(debug::ScalarType::I32);
				case DataTypeScalarCode::F32: return static_cast<u8>(debug::ScalarType::F32);
				default:                      return static_cast<u8>(debug::ScalarType::Invalid);
			}
		}
	}

	std::optional<vm::Program> BinaryEmitter::emit()
	{
		Address entryPoint = Address::Null;
		auto mainSymbol = _state.get().globalSymbolTable().get(SymbolTable::EntryPointLabelName);
		if (!mainSymbol.has_value() || !mainSymbol.value().get().isLabel() || !mainSymbol.value().get().isGlobal() || !mainSymbol.value().get().hasAddress())
		{
			reportError(0, "Entry point label '{}' is not defined or not a global label", SymbolTable::EntryPointLabelName);
			return std::nullopt;
		}
		entryPoint = mainSymbol.value().get().address();

		for (const auto& unit : _state.get().translationUnits())
		{
			std::optional<SectionType> currentSection = std::nullopt;
			const usize textStart = _textBuffer.size();
			const usize rodataStart = _rodataBuffer.size();
			const usize dataStart = _dataBuffer.size();
			for (const auto& statement : unit.ast())
			{
				_currentFile = statement.file();

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

			// Mirrors alignUp() in the linker: it advanced the next unit's offsets by an aligned
			// size, so the bytes have to be padded to the same boundary.
			padSectionToAlignment(_textBuffer, textStart);
			padSectionToAlignment(_rodataBuffer, rodataStart);
			padSectionToAlignment(_dataBuffer, dataStart);
		}

		vm::ProgramHeader header{
			.magic = vm::ProgramHeader::MagicNumber,
			.version = vm::ProgramHeader::CurrentVersion,
			.entryPoint = entryPoint.value(),
			.textSize = _state.get().memoryMap().textSize,
			.rodataSize = _state.get().memoryMap().rodataSize,
			.dataSize = _state.get().memoryMap().dataSize,
			.bssSize = _state.get().memoryMap().bssSize,
			.minimumStack = 1024 // For now, we can set this to 1024. In the future, we might want to calculate the minimum stack size based on the program's requirements.
		};

		std::vector<u8> debugSection;
		if (_emitDebugInfo)
		{
			recordDebugSymbols();
			_debugInfo = _debugBuilder.release();
			debugSection = _debugInfo.serialize();
		}

		return vm::Program::make(
			header,
			_textBuffer,
			_rodataBuffer,
			_dataBuffer,
			debugSection
		);
	}

	// Addresses here are the ones the linker handed out, already relocated to where the loader will
	// place each section, so nothing downstream has to adjust them.
	void BinaryEmitter::recordDebugSymbols()
	{
		for (const auto& unit : _state.get().translationUnits())
		{
			for (const auto& [name, symbol] : unit.symbolTable().getAllSymbols())
			{
				debug::SymbolEntry entry;
				entry.nameOffset = _debugBuilder.internString(name);
				entry.address = symbol.hasAddress() ? symbol.address().value() : 0;

				switch (symbol.type())
				{
					case SymbolType::Label:    entry.kind = static_cast<u8>(debug::SymbolKind::Label); break;
					case SymbolType::Constant: entry.kind = static_cast<u8>(debug::SymbolKind::Constant); break;
					case SymbolType::Variable: entry.kind = static_cast<u8>(debug::SymbolKind::Variable); break;
				}

				// A constant occupies no memory, so it belongs to no section however the symbol
				// table happens to have tagged it.
				if (symbol.isConstant())
				{
					entry.section = static_cast<u8>(debug::SymbolSection::None);
				}
				else
				{
					switch (symbol.section())
					{
						case SectionType::Text:   entry.section = static_cast<u8>(debug::SymbolSection::Text); break;
						case SectionType::Rodata: entry.section = static_cast<u8>(debug::SymbolSection::Rodata); break;
						case SectionType::Data:   entry.section = static_cast<u8>(debug::SymbolSection::Data); break;
						case SectionType::BSS:    entry.section = static_cast<u8>(debug::SymbolSection::BSS); break;
					}
				}

				if (symbol.isGlobal())
					entry.flags |= debug::SymbolFlag::Global;
				if (symbol.isReadonly())
					entry.flags |= debug::SymbolFlag::Readonly;

				if (symbol.hasDataType())
				{
					const DataType dataType = symbol.dataType();
					entry.scalarType = toDebugScalarType(dataType.scalarCode());
					entry.elementCount = dataType.numElements();
					entry.size = dataType.sizeInBytes().value_or(0);
				}

				// Only a scalar constant carries a value a debugger can show; an array one would
				// need the whole literal, which is not worth a variable-length record here.
				if (symbol.isConstant() && symbol.hasValue() && symbol.value().isScalar())
				{
					const LiteralScalar& scalar = symbol.value().elements().front();
					entry.scalarType = toDebugScalarType(scalar.scalarCode());
					entry.value = scalar.rawBits();
					entry.flags |= debug::SymbolFlag::HasValue;
				}

				_debugBuilder.addSymbol(entry);
			}
		}
	}

	void BinaryEmitter::recordDebugLine(const RelocatableStatement& statement, Address address, u16 flags)
	{
		debug::LineEntry entry;
		entry.address = address.value();
		entry.fileId = _debugBuilder.internFile(statement.file());
		entry.line = statement.line();
		entry.expansionFileId = _debugBuilder.internFile(statement.expansionFile());
		entry.expansionLine = statement.expansionLine();
		entry.macroDepth = statement.macroDepth();
		entry.flags = flags;

		if (statement.macroDepth() > 0)
			entry.flags |= debug::LineFlag::MacroExpansion;

		_debugBuilder.addLine(entry);
	}

	// Mirrors alignCurrentOffset() in the translation unit: both have to insert the same padding
	// or the bytes drift away from the addresses the linker handed out.
	// Pads whatever a single unit contributed up to the section alignment.
	void BinaryEmitter::padSectionToAlignment(std::vector<u8>& buffer, usize unitStart)
	{
		const usize contributed = buffer.size() - unitStart;
		const usize remainder = contributed % SectionAlignment;
		if (remainder != 0)
			buffer.resize(buffer.size() + (SectionAlignment - remainder), 0);
	}

	void BinaryEmitter::padToAlignment(std::vector<u8>& buffer, u32 alignment)
	{
		if (alignment <= 1)
			return;

		const usize misaligned = buffer.size() % alignment;
		if (misaligned != 0)
			buffer.resize(buffer.size() + (alignment - misaligned), 0);
	}

	void BinaryEmitter::emitData(const RelocatableStatement& statement, bool isRodata)
	{
		const ResolvedDataStatement& data = statement.asData();
		std::vector<u8>& buffer = isRodata ? _rodataBuffer : _dataBuffer;

		padToAlignment(buffer, data.dataType.alignment());
		
		if (data.value.has_value())
		{
			const LiteralValue& value = data.value.value();
			DataType type = data.dataType;
			if (value.hasUnknownSize())
			{
				reportError(statement.line(), "Data statement has an initial value with unknown size");
				return;
			}

			u32 size = 0;
			if (type.hasUnknownSize())
				size = type.withNumElements(static_cast<u32>(value.elements().size())).sizeInBytes().value_or(0);
			else
				size = type.sizeInBytes().value_or(0);

			if (size == 0)
			{
				reportError(statement.line(), "Cannot determine size of data statement");
				return;
			}

			const usize bufferStart = buffer.size();

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

			// A declaration may be larger than its initialiser (`let buf: u8[64] = "hi"`). The linker
			// already reserved the declared size, so the remainder has to be written out as zeroes or
			// every later symbol sits at the wrong address.
			const usize written = buffer.size() - bufferStart;
			if (written > size)
			{
				reportError(statement.line(), "Data statement emitted {} bytes but only {} were reserved", written, size);
				return;
			}
			if (written < size)
				buffer.resize(bufferStart + size, 0);
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

	namespace
	{
		// The raw value an operand contributes to an immediate field, before shifting.
		std::optional<u32> immediateSourceValue(const Operand& operand) noexcept
		{
			if (operand.isVariable())
				return operand.asVariable().address.value();
			if (operand.isLabel())
				return operand.asLabel().address.value();
			if (operand.isImmediate())
				return operand.asImmediate().value;
			return std::nullopt;
		}

		// True when truncating to `bits` loses nothing, reading the value as either signed or
		// unsigned. Same rule the literal narrowing uses, so `-1` fits a byte and `70000` does not.
		constexpr bool fitsInBits(u32 value, u32 bits) noexcept
		{
			if (bits >= 32)
				return true;

			const u32 mask = (1u << bits) - 1u;
			const u32 truncated = value & mask;
			const u32 signExtended = (truncated & (1u << (bits - 1))) != 0 ? (truncated | ~mask) : truncated;

			return value == truncated || value == signExtended;
		}

		constexpr u32 immediateFieldWidth(OpcodeParameterType type) noexcept
		{
			switch (type)
			{
				case OpcodeParameterType::IMM8: return 8;
				case OpcodeParameterType::IMM16:
				case OpcodeParameterType::SIMM16: return 16;
				case OpcodeParameterType::IMM24:
				case OpcodeParameterType::SIMM24:
				case OpcodeParameterType::REL_ADDR: return 24;
				default: return 32;
			}
		}

		constexpr std::string_view immediateFieldName(OpcodeParameterType type) noexcept
		{
			switch (type)
			{
				case OpcodeParameterType::IMM8: return "an 8-bit immediate";
				case OpcodeParameterType::IMM16: return "a 16-bit immediate";
				case OpcodeParameterType::SIMM16: return "a signed 16-bit immediate";
				case OpcodeParameterType::IMM24: return "a 24-bit immediate";
				case OpcodeParameterType::SIMM24: return "a signed 24-bit immediate";
				case OpcodeParameterType::REL_ADDR: return "a 24-bit relative displacement";
				default: return "an immediate";
			}
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
						case OpcodeParameterType::IMM16:
						case OpcodeParameterType::SIMM16:
						case OpcodeParameterType::IMM24:
						case OpcodeParameterType::SIMM24:
						{
							const auto sourceValue = immediateSourceValue(operandInfo);
							if (!sourceValue.has_value())
							{
								reportError(statement.line(), "Operand {} cannot supply an immediate value", operandIndex);
								return;
							}

							const u32 shifted = *sourceValue >> param.fixedValueShift();
							const u32 width = immediateFieldWidth(param.type());

							// Truncating here used to be silent: `li r0, 70000` quietly became `li r0, 4464`.
							if (!fitsInBits(shifted, width))
							{
								reportError(statement.line(), "Value {} does not fit in {}", shifted, immediateFieldName(param.type()));
								return;
							}

							switch (param.type())
							{
								case OpcodeParameterType::IMM8:   encodedInstruction.setImm8(static_cast<u8>(shifted)); break;
								case OpcodeParameterType::IMM16:  encodedInstruction.setImm16(static_cast<u16>(shifted)); break;
								case OpcodeParameterType::SIMM16: encodedInstruction.setSImm16(static_cast<i16>(shifted)); break;
								case OpcodeParameterType::IMM24:  encodedInstruction.setImm24(static_cast<u24>(shifted)); break;
								default:                          encodedInstruction.setSImm24(static_cast<i24>(shifted)); break;
							}
							break;
						}

						case OpcodeParameterType::RD_IMM16:
						{
							const MemoryOperand& memoryOperand = operandInfo.asMemory();
							encodedInstruction.setRd(memoryOperand.baseRegIndex);

							if (memoryOperand.isImmediateOffset())
								encodedInstruction.setImm16(static_cast<u16>(memoryOperand.immediateOffset().value));
							else if (memoryOperand.isIdentifierOffset())
							{
								// The linker rewrites symbolic offsets into immediates, so reaching this
								// point means the symbol was never resolved.
								reportError(statement.line(), "Unresolved symbolic offset '{}' in memory operand", memoryOperand.identifierOffset().name.view());
								return;
							}
							// No offset at all (e.g. [r1]) leaves imm16 at zero.
							break;
						}

						case OpcodeParameterType::RS_IMM16:
						{
							const MemoryOperand& memoryOperand = operandInfo.asMemory();
							encodedInstruction.setRs(memoryOperand.baseRegIndex);

							if (memoryOperand.isImmediateOffset())
								encodedInstruction.setImm16(static_cast<u16>(memoryOperand.immediateOffset().value));
							else if (memoryOperand.isIdentifierOffset())
							{
								// The linker rewrites symbolic offsets into immediates, so reaching this
								// point means the symbol was never resolved.
								reportError(statement.line(), "Unresolved symbolic offset '{}' in memory operand", memoryOperand.identifierOffset().name.view());
								return;
							}
							// No offset at all (e.g. [r1]) leaves imm16 at zero.
							break;
						}

						case OpcodeParameterType::RT_IMM16:
						{
							const MemoryOperand& memoryOperand = operandInfo.asMemory();
							encodedInstruction.setRt(memoryOperand.baseRegIndex);

							if (memoryOperand.isImmediateOffset())
								encodedInstruction.setImm16(static_cast<u16>(memoryOperand.immediateOffset().value));
							else if (memoryOperand.isIdentifierOffset())
							{
								// The linker rewrites symbolic offsets into immediates, so reaching this
								// point means the symbol was never resolved.
								reportError(statement.line(), "Unresolved symbolic offset '{}' in memory operand", memoryOperand.identifierOffset().name.view());
								return;
							}
							// No offset at all (e.g. [r1]) leaves imm16 at zero.
							break;
						}

						case OpcodeParameterType::REL_ADDR:
							if (operandInfo.isLabel())
							{
								Address currentAddress = lastSectionAddress(SectionType::Text);
								Address targetAddress = operandInfo.asLabel().address;
								i32 relativeOffset = static_cast<i32>(targetAddress.value()) - static_cast<i32>(currentAddress.value());

								// simm24 reaches +/- 8 MiB. Past that the displacement wraps and the branch
								// lands somewhere arbitrary.
								if (!fitsInBits(static_cast<u32>(relativeOffset), 24))
								{
									reportError(statement.line(), "Branch target is {} bytes away, out of range for a 24-bit displacement", relativeOffset);
									return;
								}

								encodedInstruction.setSImm24(static_cast<i24>(relativeOffset));
							}
							else // if (operandInfo.isImmediate())
							{
								encodedInstruction.setSImm24(static_cast<i24>(operandInfo.asImmediate().value));
							}
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
			if (_emitDebugInfo)
				recordDebugLine(statement, lastSectionAddress(SectionType::Text), debug::LineFlag::None);
			writeToBuffer(_textBuffer, encodedInstruction);
		}

		while (remainingOpcodes > 0)
		{
			// Filler for the size the assembler reserved before it knew which overload would be
			// chosen. Marked so a debugger can step straight through instead of stopping on a NOP
			// the programmer never wrote.
			if (_emitDebugInfo)
				recordDebugLine(statement, lastSectionAddress(SectionType::Text), debug::LineFlag::PseudoPadding);
			writeToBuffer(_textBuffer, vm::Instruction::NOP());
			remainingOpcodes--;
		}
	}

	Address BinaryEmitter::lastSectionAddress(SectionType sectionType)
	{
		switch (sectionType)
		{
			case SectionType::Text:
				return _state.get().memoryMap().textStart + static_cast<u32>(_textBuffer.size());

			case SectionType::Rodata:
				return _state.get().memoryMap().rodataStart + static_cast<u32>(_rodataBuffer.size());

			case SectionType::Data:
				return _state.get().memoryMap().dataStart + static_cast<u32>(_dataBuffer.size());

			default:
				reportError(0, "Unsupported section type for lastSectionAddress");
				return Address::Null;
		}
	}
}
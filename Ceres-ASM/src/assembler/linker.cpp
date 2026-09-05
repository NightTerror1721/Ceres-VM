#include "linker.h"
#include "vm/memory.h"

namespace ceres::casm
{
	bool Linker::link()
	{
		calculateMemoryMap();
		MemoryOffsets offsets = calculateMemoryOffsets();
		MemoryMap& memoryMap = _state.get().memoryMap();
		SymbolTable& globalSymbolTable = _state.get().globalSymbolTable();

		// Relocate symbols in each translation unit and build the global symbol table
		for (auto& unit : _state.get().translationUnits())
		{
			unit.symbolTable().relocateSymbols(offsets.textOffset, offsets.dataOffset, offsets.rodataOffset, offsets.bssOffset);

			for (const auto& [name, symbol] : unit.symbolTable().getAllSymbols())
			{
				if (symbol.isGlobal())
				{
					if (globalSymbolTable.get(name).has_value())
					{
						reportError(0, "Linker error: Symbol '{}' is defined in multiple translation units.", name);
						continue; // Skip adding this symbol to the global symbol table
					}
					globalSymbolTable.insertRawSymbol(symbol);
				}
			}

			offsets.textOffset += unit.sectionSizes().textSize;
			offsets.rodataOffset += unit.sectionSizes().rodataSize;
			offsets.dataOffset += unit.sectionSizes().dataSize;
			offsets.bssOffset += unit.sectionSizes().bssSize;
		}

		// Check for unresolved symbols in each translation unit
		for (const auto& unit : _state.get().translationUnits())
		{
			for (const auto& unresolvedSymbol : unit.unresolvedSymbols())
			{
				if (unresolvedSymbol.isLocal())
				{
					if (unit.symbolTable().getLocal(unresolvedSymbol.name, unresolvedSymbol.parentName).has_value())
						continue;

					reportError(unresolvedSymbol.line, "Linker error: Unresolved local symbol '.{}'.", unresolvedSymbol.name);
					continue; // Skip adding this symbol to the global symbol table
				}
				else
				{
					if (unit.symbolTable().get(unresolvedSymbol.name).has_value())
						continue;

					if (globalSymbolTable.get(unresolvedSymbol.name).has_value())
						continue;

					reportError(unresolvedSymbol.line, "Linker error: Unresolved symbol '{}'.", unresolvedSymbol.name);
					continue; // Skip adding this symbol to the global symbol table
				}
			}
		}

		// Resolve operands in each translation unit using the global symbol table
		for (auto& unit : _state.get().translationUnits())
		{
			std::string_view lastParentLabel = {};
			for (auto& statement : unit.ast())
			{
				try
				{
					if (statement.isLabel())
					{
						const auto& label = statement.asLabel();
						if (label.level != LabelLevel::Local)
							lastParentLabel = label.name;
					}
					if (statement.isInstruction())
					{
						InstructionStatement& instructionStatement = statement.asInstruction();
						for (auto& operand : instructionStatement.operands)
							unit.symbolTable().resolveOperand(statement.line(), operand, lastParentLabel, globalSymbolTable);

						auto info = InstructionInfo::find(instructionStatement.signature());
						if (!info.has_value())
							error(statement.line(), "Invalid instruction syntax: {}", instructionStatement.signature().toString());
					}
				}
				catch (const AssemblerError& ex)
				{
					reportError(ex.line(), "Linker error: {}", ex.what());
				}
			}
		}

		return !_state.get().errorHandler().hasErrors();
	}

	void Linker::calculateMemoryMap() const
	{
		MemoryMap& memoryMap = _state.get().memoryMap();
		memoryMap.textSize = 0;
		memoryMap.dataSize = 0;
		memoryMap.rodataSize = 0;
		memoryMap.bssSize = 0;

		for (const auto& unit : _state.get().translationUnits())
		{
			const SectionSizes& sizes = unit.sectionSizes();
			memoryMap.textSize += sizes.textSize;
			memoryMap.dataSize += sizes.dataSize;
			memoryMap.rodataSize += sizes.rodataSize;
			memoryMap.bssSize += sizes.bssSize;
		}

		
		memoryMap.textStart = vm::Memory::UnrestrictedSegmentStart;
		memoryMap.rodataStart = memoryMap.textStart + memoryMap.textSize;
		memoryMap.dataStart = memoryMap.rodataStart + memoryMap.rodataSize;
		memoryMap.bssStart = memoryMap.dataStart + memoryMap.dataSize;
	}

	Linker::MemoryOffsets Linker::calculateMemoryOffsets() const
	{
		const MemoryMap& memoryMap = _state.get().memoryMap();
		return MemoryOffsets{
			.textOffset = memoryMap.textStart,
			.rodataOffset = memoryMap.rodataStart,
			.dataOffset = memoryMap.dataStart,
			.bssOffset = memoryMap.bssStart
		};
	}
}
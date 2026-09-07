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
			_currentFile = unit.file();
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

			offsets.textOffset += alignUp(unit.sectionSizes().textSize);
			offsets.rodataOffset += alignUp(unit.sectionSizes().rodataSize);
			offsets.dataOffset += alignUp(unit.sectionSizes().dataSize);
			offsets.bssOffset += alignUp(unit.sectionSizes().bssSize);
		}

		// Check for unresolved symbols in each translation unit
		for (const auto& unit : _state.get().translationUnits())
		{
			for (const auto& unresolvedSymbol : unit.unresolvedSymbols())
			{
				// More precise than unit.file(): a macro-expanded reference's own file can differ
				// from the file of the unit its expansion ended up in.
				_currentFile = unresolvedSymbol.file;

				if (unresolvedSymbol.isLocal())
				{
					if (unit.symbolTable().getLocal(unresolvedSymbol.name, unresolvedSymbol.parentName).has_value())
						continue;

					reportError(unresolvedSymbol.line, "Linker error: Unresolved local symbol '.{}'.", unresolvedSymbol.name);
					continue; // Skip adding this symbol to the global symbol table
				}
				else
				{
					// The unit's own table, then whatever its imports export, then the global table
					// the pass above filled from every unit.
					if (unit.resolveSymbol(unresolvedSymbol.name).has_value())
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
				_currentFile = statement.file();
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
							unit.symbolTable().resolveOperand(statement.line(), operand, lastParentLabel, globalSymbolTable, &unit);

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
			// Each unit contributes a whole number of aligned blocks, so the unit that follows it
			// starts aligned too.
			const SectionSizes& sizes = unit.sectionSizes();
			memoryMap.textSize += alignUp(sizes.textSize);
			memoryMap.dataSize += alignUp(sizes.dataSize);
			memoryMap.rodataSize += alignUp(sizes.rodataSize);
			memoryMap.bssSize += alignUp(sizes.bssSize);
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
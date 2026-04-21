#include "linker.h"
#include "vm/memory.h"

namespace ceres::casm
{
	std::optional<LinkedExecutable> Linker::link(std::vector<TranslationUnit>&& units)
	{
		MemoryMap memoryMap = calculateMemoryMap(units);
		MemoryOffsets offsets = calculateMemoryOffsets(memoryMap);
		SymbolTable globalSymbolTable;

		// Relocate symbols in each translation unit and build the global symbol table
		for (auto& unit : units)
		{
			unit.symbolTable().relocateSymbols(offsets.textOffset, offsets.dataOffset, offsets.rodataOffset, offsets.bssOffset);

			for (const auto& [name, symbol] : unit.symbolTable().getAllSymbols())
			{
				if (symbol.isGlobal())
				{
					if (globalSymbolTable.get(name).has_value())
					{
						reportError(0, "Linker error: Symbol '{}' is defined in multiple translation units.", name);
						return std::nullopt;
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
		for (const auto& unit : units)
		{
			for (const auto& symbolName : unit.unresolvedSymbols())
			{
				Identifier identifier = Identifier::make(symbolName);
				if (unit.symbolTable().get(symbolName).has_value())
					continue;

				if (globalSymbolTable.get(symbolName).has_value())
					continue;

				reportError(0, "Linker error: Unresolved symbol '{}'.", symbolName);
				return std::nullopt;
			}
		}

		// Resolve operands in each translation unit using the global symbol table
		for (auto& unit : units)
		{
			for (auto& statement : unit.ast())
			{
				try
				{
					if (statement.isInstruction())
					{
						InstructionStatement& instructionStatement = statement.asInstruction();
						for (auto& operand : instructionStatement.operands)
							unit.symbolTable().resolveOperand(statement.line(), operand, globalSymbolTable);

						auto info = InstructionInfo::find(instructionStatement.signature());
						if (!info.has_value())
							error(statement.line(), "Invalid instruction syntax: {}", instructionStatement.signature().toString());
					}
				}
				catch (const AssemblerError& e)
				{
					reportError(e.line(), "Linker error: {}", e.what());
				}
			}
		}

		return LinkedExecutable(std::move(units), std::move(globalSymbolTable), std::move(memoryMap));
	}

	MemoryMap Linker::calculateMemoryMap(const std::vector<TranslationUnit>& units) const
	{
		MemoryMap memoryMap;

		for (const auto& unit : units)
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

		return memoryMap;
	}

	Linker::MemoryOffsets Linker::calculateMemoryOffsets(const MemoryMap& memoryMap) const
	{
		return MemoryOffsets{
			.textOffset = memoryMap.textStart,
			.rodataOffset = memoryMap.rodataStart,
			.dataOffset = memoryMap.dataStart,
			.bssOffset = memoryMap.bssStart
		};
	}
}
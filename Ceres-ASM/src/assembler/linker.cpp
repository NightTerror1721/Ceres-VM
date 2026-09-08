#include "linker.h"
#include "vm/memory.h"

namespace ceres::casm
{
	// An instruction's size is fixed before anything knows where the variable it names will end
	// up: the build pass adds section sizes as it walks, and needs each size to do it. So LDV and
	// STV reserve three words whether or not the one-word LDVP/STVP would reach.
	//
	// Relaxation is the usual answer, and it usually needs a fixpoint: shorten, lay out again,
	// find something that no longer reaches, grow it back. Here it needs exactly one pass, because
	// the layout puts all of .text before all of .rodata, .data and .bss - so every reference from
	// an instruction to a variable points forward.
	//
	// Take an instruction at A naming a variable at D > A. Shortening instructions *before* A
	// lowers A and D by the same amount and the distance is unchanged; shortening instructions
	// *after* A lowers only D and the distance shrinks. Shortening never moves anything further
	// away. So whatever reaches on the pessimistic layout still reaches once everything has been
	// shortened, and measuring once is enough.
	bool Linker::link()
	{
		// Captured before the first relocation makes these absolute, so the second pass can start
		// from the same place the first one did.
		const LinkSnapshot snapshot = capture();

		if (!linkOnce())
			return false;

		if (!relaxInstructions())
			return true;

		restore(snapshot);
		for (auto& unit : _state.get().translationUnits())
			unit.relayoutText();

		// linkOnce fills this from the units, and defineLinkerSymbols refuses to write a name that
		// is already there.
		_state.get().globalSymbolTable().clear();

		return linkOnce();
	}

	Linker::LinkSnapshot Linker::capture() const
	{
		LinkSnapshot snapshot;
		for (const auto& unit : _state.get().translationUnits())
		{
			auto& addresses = snapshot.symbolAddresses.emplace_back();
			for (const auto& [name, symbol] : unit.symbolTable().getAllSymbols())
			{
				if (symbol.hasAddress())
					addresses.emplace_back(name, symbol.address());
			}

			auto& operands = snapshot.operands.emplace_back();
			for (const auto& statement : unit.ast())
			{
				if (statement.isInstruction())
					operands.push_back(statement.asInstruction().operands);
			}
		}
		return snapshot;
	}

	void Linker::restore(const LinkSnapshot& snapshot)
	{
		usize unitIndex = 0;
		for (auto& unit : _state.get().translationUnits())
		{
			if (unitIndex >= snapshot.symbolAddresses.size())
				break;

			for (const auto& [name, address] : snapshot.symbolAddresses[unitIndex])
				unit.symbolTable().updateAddress(name, address);

			const auto& operands = snapshot.operands[unitIndex];
			usize statementIndex = 0;
			for (auto& statement : unit.ast())
			{
				if (!statement.isInstruction())
					continue;
				if (statementIndex >= operands.size())
					break;

				// The mnemonic is deliberately left alone: rewriting it is what relaxation did, and
				// it is the one thing the second pass has to keep.
				statement.asInstruction().operands = operands[statementIndex];
				++statementIndex;
			}

			++unitIndex;
		}
	}

	bool Linker::relaxInstructions()
	{
		const MemoryMap& memoryMap = _state.get().memoryMap();
		Address textBase = memoryMap.textStart;
		bool changed = false;

		for (auto& unit : _state.get().translationUnits())
		{
			for (auto& statement : unit.ast())
			{
				if (!statement.isInstruction() || !statement.hasAddress())
					continue;

				InstructionStatement& instruction = statement.asInstruction();
				const auto shortForm = InstructionInfo::shortFormOf(instruction.mnemonic);
				if (!shortForm.has_value())
					continue;

				// Both forms take the register and then the variable. An operand that is not a
				// resolved variable is one the emitter is about to complain about anyway.
				if (instruction.operands.size() != 2 || !instruction.operands[1].isVariable())
					continue;

				const i64 target = static_cast<i64>(instruction.operands[1].asVariable().address.value());
				const i64 here = static_cast<i64>((textBase + statement.address()).value());
				const i64 displacement = target - here;

				// The same signed 16-bit field the emitter will encode it into.
				if (displacement < -32768 || displacement > 32767)
					continue;

				instruction.mnemonic = shortForm.value();
				changed = true;
			}

			textBase += alignUp(unit.sectionSizes().textSize);
		}

		return changed;
	}

	bool Linker::linkOnce()
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
				// Only what actually has to be linked goes here: labels and variables, which have
				// addresses another unit needs. A constant occupies no memory and is substituted at
				// its use, so it travels by import instead - and publishing it here would make two
				// libraries that each declare `global const MAX` collide even when no file uses both.
				if (symbol.isGlobal() && !symbol.isConstant())
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

		defineLinkerSymbols();

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

					// A name that is declared somewhere in the import graph but not exported is the
					// common mistake, and "Unresolved symbol" gives no hint about it.
					if (const std::string_view origin = unit.findUnexportedSymbolOrigin(unresolvedSymbol.name); !origin.empty())
					{
						reportError(unresolvedSymbol.line,
							"'{}' is declared in '{}' but is not global, so it is not visible here.",
							unresolvedSymbol.name, origin);
						continue;
					}

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

	// Where the layout turned out to put things. A program had no way to ask any of this and no
	// arithmetic that could work it out: section sizes depend on every unit in the link, so even
	// `sizeof` over everything a file declares does not add up to the answer. Placing a heap, or
	// sizing a buffer against whatever memory is left, starts here.
	//
	// Not `__stack_top`: the stack starts at the top of memory, and how much memory there is is a
	// property of the machine the program is run on, chosen with `--memory` long after the link.
	// A program that wants it reads `sp` on entry, before anything has pushed.
	void Linker::defineLinkerSymbols()
	{
		const MemoryMap& map = _state.get().memoryMap();
		SymbolTable& globals = _state.get().globalSymbolTable();

		const struct { const char* name; SectionType section; Address address; } defined[] = {
			{ "__text_start",   SectionType::Text,   map.textStart },
			{ "__text_end",     SectionType::Text,   map.textStart + map.textSize },
			{ "__rodata_start", SectionType::Rodata, map.rodataStart },
			{ "__rodata_end",   SectionType::Rodata, map.rodataStart + map.rodataSize },
			{ "__data_start",   SectionType::Data,   map.dataStart },
			{ "__data_end",     SectionType::Data,   map.dataStart + map.dataSize },
			{ "__bss_start",    SectionType::BSS,    map.bssStart },
			{ "__bss_end",      SectionType::BSS,    map.bssStart + map.bssSize },
			// The same address as __bss_end, under the name that says what it is for: everything
			// from here up is free ground, with the stack coming down to meet it.
			{ "__heap_start",   SectionType::BSS,    map.bssStart + map.bssSize },
		};

		for (const auto& entry : defined)
		{
			if (globals.get(entry.name).has_value())
			{
				reportError(0, "Linker error: '{}' is defined by the linker, so a program cannot declare it.", entry.name);
				continue;
			}
			globals.insertRawSymbol(Symbol::makeLabel(std::string(entry.name), entry.section, entry.address, true));
		}
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
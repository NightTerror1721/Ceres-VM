#include <ceres/asm/object_linker.h>
#include <ceres/asm/symbol_table.h>
#include <ceres/core/isa/instructions.h>
#include <ceres/core/format/memory_map.h>
#include <algorithm>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace ceres::casm
{
	namespace
	{
		constexpr u32 SectionAlignment = 4;

		constexpr u32 alignUp(u32 value) noexcept
		{
			const u32 remainder = value % SectionAlignment;
			return remainder == 0 ? value : value + (SectionAlignment - remainder);
		}

		// Where one object's four sections ended up. Everything else here is arithmetic on these.
		struct Placement
		{
			u32 text = 0;
			u32 rodata = 0;
			u32 data = 0;
			u32 bss = 0;
		};

		// The same rule the in-memory linker uses, so a program built either way has the same
		// shape: whether a value survives the field it is written into.
		constexpr bool fitsInBits(u32 value, u32 bits) noexcept
		{
			if (bits >= 32)
				return true;
			const u32 mask = (1u << bits) - 1;
			const u32 truncated = value & mask;
			if (truncated == value)
				return true;
			// Read back as a signed field: -1 fits in eight bits, 70000 does not.
			const u32 signExtended = (truncated & (1u << (bits - 1))) != 0 ? (truncated | ~mask) : truncated;
			return signExtended == value;
		}

		std::string_view nameOfSection(SectionType section) noexcept
		{
			switch (section)
			{
				case SectionType::Text:   return "text";
				case SectionType::Rodata: return "rodata";
				case SectionType::Data:   return "data";
				default:                  return "bss";
			}
		}

		u32 baseOf(const Placement& placement, SectionType section) noexcept
		{
			switch (section)
			{
				case SectionType::Text:   return placement.text;
				case SectionType::Rodata: return placement.rodata;
				case SectionType::Data:   return placement.data;
				default:                  return placement.bss;
			}
		}
	}

	std::optional<Program> ObjectLinker::link(std::vector<ObjectArchive::Member> inputs, const ObjectLinkOptions& options)
	{
		_errors.clear();
		_bytesCollected = 0;

		if (inputs.empty())
		{
			reportError("Nothing to link: no object files were given");
			return std::nullopt;
		}

		// --- Which members are in ----------------------------------------------------------------
		//
		// Everything named on the command line, plus whichever archive members answer a name that
		// is still open. Pulling one in can open new names of its own, so this goes round until a
		// pass adds nothing.
		std::vector<bool> selected(inputs.size(), false);
		for (usize i = 0; i < inputs.size(); ++i)
			selected[i] = !inputs[i].fromArchive;

		const auto definedNames = [&]() -> std::unordered_set<std::string>
		{
			std::unordered_set<std::string> names;
			for (usize i = 0; i < inputs.size(); ++i)
			{
				if (!selected[i])
					continue;
				for (const ObjectSymbol& symbol : inputs[i].object.symbols)
					names.insert(symbol.name);
			}
			return names;
		};

		for (bool changed = true; changed; )
		{
			changed = false;
			const std::unordered_set<std::string> defined = definedNames();

			std::unordered_set<std::string> wanted;
			for (usize i = 0; i < inputs.size(); ++i)
			{
				if (!selected[i])
					continue;
				for (const Relocation& relocation : inputs[i].object.relocations)
				{
					if (relocation.isExternal() && !defined.contains(relocation.symbol))
						wanted.insert(relocation.symbol);
				}
				for (const ObjectInterruptBinding& binding : inputs[i].object.interruptBindings)
				{
					if (binding.isExternal() && !defined.contains(binding.symbol))
						wanted.insert(binding.symbol);
				}
			}

			if (wanted.empty())
				break;

			for (usize i = 0; i < inputs.size() && !changed; ++i)
			{
				if (selected[i])
					continue;

				const bool answers = std::ranges::any_of(inputs[i].object.symbols,
					[&](const ObjectSymbol& symbol) { return wanted.contains(symbol.name); });
				if (answers)
				{
					selected[i] = true;
					changed = true;
				}
			}
		}

		std::vector<const ObjectArchive::Member*> members;
		{
			std::vector<ObjectArchive::Member*> chosen;
			for (usize i = 0; i < inputs.size(); ++i)
			{
				if (selected[i])
					chosen.push_back(&inputs[i]);
			}
			if (options.gcSections && !options.emitDebugInfo)
				collectUnusedCode(chosen);
			members.assign(chosen.begin(), chosen.end());
		}

		// --- Where everything goes ---------------------------------------------------------------
		//
		// All the code, then all the read-only data, then the writable data, then the space that is
		// only zeroed - the same order the whole-program linker lays out, so a program built either
		// way has the same map.
		u32 textSize = 0;
		u32 rodataSize = 0;
		u32 dataSize = 0;
		u32 bssSize = 0;
		for (const ObjectArchive::Member* member : members)
		{
			textSize += alignUp(static_cast<u32>(member->object.text.size()));
			rodataSize += alignUp(static_cast<u32>(member->object.rodata.size()));
			dataSize += alignUp(static_cast<u32>(member->object.data.size()));
			bssSize += alignUp(member->object.bssSize);
		}

		// --symtab: every global name in .text, laid out after the members' own .rodata (object_linker.h).
		struct SymtabEntry { std::string_view name; usize member; u32 offset; };
		std::vector<SymtabEntry> symtabEntries;
		const u32 membersRodataSize = rodataSize;
		if (options.emitSymbolTable)
		{
			u32 namesSize = 0;
			for (usize i = 0; i < members.size(); ++i)
			{
				for (const ObjectSymbol& symbol : members[i]->object.symbols)
				{
					if (symbol.section != SectionType::Text)
						continue;
					symtabEntries.push_back(SymtabEntry{ symbol.name, i, symbol.offset });
					namesSize += static_cast<u32>(symbol.name.size()) + 1;
				}
			}
			rodataSize += alignUp(4 + 8 * static_cast<u32>(symtabEntries.size()) + namesSize);
		}

		const u32 textStart = fmt::MemoryMap::UnrestrictedSegmentStart.value();
		const u32 rodataStart = textStart + textSize;
		const u32 dataStart = rodataStart + rodataSize;
		const u32 bssStart = dataStart + dataSize;

		std::vector<Placement> placements(members.size());
		{
			Placement running{ textStart, rodataStart, dataStart, bssStart };
			for (usize i = 0; i < members.size(); ++i)
			{
				placements[i] = running;
				const ObjectFile& object = members[i]->object;
				running.text += alignUp(static_cast<u32>(object.text.size()));
				running.rodata += alignUp(static_cast<u32>(object.rodata.size()));
				running.data += alignUp(static_cast<u32>(object.data.size()));
				running.bss += alignUp(object.bssSize);
			}
		}

		// --- What every name means ---------------------------------------------------------------
		std::unordered_map<std::string, u32> globals;
		for (usize i = 0; i < members.size(); ++i)
		{
			for (const ObjectSymbol& symbol : members[i]->object.symbols)
			{
				const u32 address = baseOf(placements[i], symbol.section) + symbol.offset;
				const auto [entry, inserted] = globals.emplace(symbol.name, address);
				if (!inserted)
					reportError(std::format("'{}' is defined in more than one object; the second is '{}'",
						symbol.name, members[i]->name));
			}
		}

		// The addresses only a link knows, defined here for the same reason the in-memory linker
		// defines them: section sizes are a property of the whole program, and no single unit can
		// work them out.
		const std::pair<const char*, u32> linkerDefined[] = {
			{ "__text_start",   textStart },
			{ "__text_end",     textStart + textSize },
			{ "__rodata_start", rodataStart },
			{ "__rodata_end",   rodataStart + rodataSize },
			{ "__data_start",   dataStart },
			{ "__data_end",     dataStart + dataSize },
			{ "__bss_start",    bssStart },
			{ "__bss_end",      bssStart + bssSize },
			{ "__heap_start",   bssStart + bssSize },
			{ "__symtab_start", rodataStart + membersRodataSize },
			{ "__symtab_end",   rodataStart + rodataSize },
		};
		for (const auto& [name, address] : linkerDefined)
		{
			if (globals.contains(name))
			{
				reportError(std::format("'{}' is defined by the linker, so a program cannot define it", name));
				continue;
			}
			globals.emplace(name, address);
		}

		// --- The bytes ---------------------------------------------------------------------------
		std::vector<u8> text;
		std::vector<u8> rodata;
		std::vector<u8> data;
		text.reserve(textSize);
		rodata.reserve(rodataSize);
		data.reserve(dataSize);

		for (const ObjectArchive::Member* member : members)
		{
			const auto append = [](std::vector<u8>& out, const std::vector<u8>& bytes)
			{
				out.insert(out.end(), bytes.begin(), bytes.end());
				while (out.size() % SectionAlignment != 0)
					out.push_back(0);
			};

			append(text, member->object.text);
			append(rodata, member->object.rodata);
			append(data, member->object.data);
		}

		if (options.emitSymbolTable)
		{
			std::vector<std::pair<u32, std::string_view>> sorted;
			sorted.reserve(symtabEntries.size());
			for (const SymtabEntry& entry : symtabEntries)
				sorted.emplace_back(placements[entry.member].text + entry.offset, entry.name);
			std::ranges::sort(sorted);
			const auto word = [&rodata](u32 value)
			{
				for (u32 shift = 0; shift < 32; shift += 8)
					rodata.push_back(static_cast<u8>((value >> shift) & 0xFF));
			};
			word(static_cast<u32>(sorted.size()));
			u32 nameAt = 4 + 8 * static_cast<u32>(sorted.size());
			for (const auto& [address, name] : sorted)
			{
				word(address);
				word(nameAt);
				nameAt += static_cast<u32>(name.size()) + 1;
			}
			for (const auto& entry : sorted)
			{
				rodata.insert(rodata.end(), entry.second.begin(), entry.second.end());
				rodata.push_back(0);
			}
			while (rodata.size() % SectionAlignment != 0)
				rodata.push_back(0);
		}

		// --- Filling in the blanks ---------------------------------------------------------------
		std::unordered_set<std::string> reported;
		for (usize i = 0; i < members.size(); ++i)
		{
			const ObjectFile& object = members[i]->object;
			const Placement& placement = placements[i];
			const u32 objectTextStart = placement.text - textStart; // Into the merged buffer
			const u32 objectRodataStart = placement.rodata - rodataStart;
			const u32 objectDataStart = placement.data - dataStart;

			for (const Relocation& relocation : object.relocations)
			{
				u32 target = 0;
				if (relocation.isExternal())
				{
					const auto found = globals.find(relocation.symbol);
					if (found == globals.end())
					{
						// Once per name per object. Reaching a variable takes two instructions and so
						// two relocations, and that is one mistake, not two.
						if (reported.emplace(members[i]->name + ": " + relocation.symbol).second)
						{
							reportError(std::format("Undefined symbol '{}', wanted by '{}'",
								relocation.symbol, members[i]->name));
						}
						continue;
					}
					target = found->second + static_cast<u32>(relocation.addend);
				}
				else
				{
					target = baseOf(placement, relocation.section) + static_cast<u32>(relocation.addend);
				}

				// A whole 32-bit word in .rodata or .data, not an instruction: patched with the
				// address as it stands. The only field that ever appears outside .text, and the
				// only one with nothing to range-check - a Ceres address is 32 bits, and so is the
				// word holding it.
				if (relocation.patchedSection != SectionType::Text)
				{
					if (relocation.patchedSection != SectionType::Rodata && relocation.patchedSection != SectionType::Data)
					{
						reportError(std::format("{}: a relocation patches {}, which holds no bytes to patch",
							members[i]->name, nameOfSection(relocation.patchedSection)));
						continue;
					}

					const bool isRodata = relocation.patchedSection == SectionType::Rodata;
					std::vector<u8>& section = isRodata ? rodata : data;
					const u32 objectSectionStart = isRodata ? objectRodataStart : objectDataStart;
					const usize position = static_cast<usize>(objectSectionStart) + relocation.offset;
					const std::vector<u8>& ownSection = isRodata ? object.rodata : object.data;
					if (static_cast<usize>(relocation.offset) + 4 > ownSection.size())
					{
						reportError(std::format("{}: a relocation points past the end of its own {}",
							members[i]->name, nameOfSection(relocation.patchedSection)));
						continue;
					}
					section[position] = static_cast<u8>(target & 0xFF);
					section[position + 1] = static_cast<u8>((target >> 8) & 0xFF);
					section[position + 2] = static_cast<u8>((target >> 16) & 0xFF);
					section[position + 3] = static_cast<u8>((target >> 24) & 0xFF);
					continue;
				}

				const usize position = static_cast<usize>(objectTextStart) + relocation.offset;
				if (position + Instruction::Size > text.size())
				{
					reportError(std::format("{}: a relocation points past the end of its own code",
						members[i]->name));
					continue;
				}

				const u32 here = placement.text + relocation.offset;
				i64 value = relocation.pcRelative
					? static_cast<i64>(target) - static_cast<i64>(here)
					: static_cast<i64>(target);
				if (!relocation.pcRelative)
					value = static_cast<i64>(static_cast<u32>(value) >> relocation.shift);

				Instruction instruction(
					static_cast<Instruction::RawType>(text[position]) |
					(static_cast<Instruction::RawType>(text[position + 1]) << 8) |
					(static_cast<Instruction::RawType>(text[position + 2]) << 16) |
					(static_cast<Instruction::RawType>(text[position + 3]) << 24));

				// The same checks the emitter makes when it can see the address itself. What it
				// would have refused to encode, this refuses to patch in.
				const auto tooFar = [&](std::string_view what)
				{
					reportError(std::format("{}: '{}' is {} bytes away, out of reach for {}",
						members[i]->name,
						relocation.symbol.empty() ? std::string(nameOfSection(relocation.section)) : relocation.symbol,
						value, what));
				};

				switch (relocation.field)
				{
					case RelocationField::Imm8:
						if (!fitsInBits(static_cast<u32>(value), 8)) { tooFar("an 8-bit field"); continue; }
						instruction.setImm8(static_cast<u8>(value));
						break;

					case RelocationField::Imm16:
						if (!fitsInBits(static_cast<u32>(value), 16)) { tooFar("a 16-bit field"); continue; }
						instruction.setImm16(static_cast<u16>(value));
						break;

					case RelocationField::Imm16Low:
						// Deliberately unchecked: the upper half went into the paired LUI.
						instruction.setImm16(static_cast<u16>(static_cast<u32>(value) & 0xFFFFu));
						break;

					case RelocationField::SImm16:
						if (value < -32768 || value > 32767) { tooFar("a PC-relative 16-bit displacement; use ldv/stv instead"); continue; }
						instruction.setSImm16(static_cast<i16>(value));
						break;

					case RelocationField::SImm20:
						if (value < -524288 || value > 524287) { tooFar("a 20-bit displacement; use call instead"); continue; }
						instruction.setSImm20(static_cast<i32>(value));
						break;

					case RelocationField::Imm24:
						if (!fitsInBits(static_cast<u32>(value), 24)) { tooFar("a 24-bit field"); continue; }
						instruction.setImm24(static_cast<u24>(static_cast<u32>(value)));
						break;

					case RelocationField::SImm24:
						if (!fitsInBits(static_cast<u32>(static_cast<i32>(value)), 24)) { tooFar("a 24-bit displacement"); continue; }
						instruction.setSImm24(static_cast<i24>(static_cast<i32>(value)));
						break;

					case RelocationField::Word32:
						// A Word32 patches a whole word in .rodata or .data, which is handled above,
						// so reaching .text means a malformed object. Present to keep the switch
						// exhaustive, and to make that corruption loud rather than silently ignored.
						reportError(std::format("{}: a Word32 relocation patches {} with no word to patch",
							members[i]->name, nameOfSection(relocation.patchedSection)));
						break;
				}

				const Instruction::RawType raw = instruction.raw();
				text[position] = static_cast<u8>(raw & 0xFF);
				text[position + 1] = static_cast<u8>((raw >> 8) & 0xFF);
				text[position + 2] = static_cast<u8>((raw >> 16) & 0xFF);
				text[position + 3] = static_cast<u8>((raw >> 24) & 0xFF);
			}
		}

		// --- Interrupt vectors -------------------------------------------------------------------
		//
		// Each object recorded its own `interrupt` bindings as an ObjectInterruptBinding: a number
		// (already final - it is a constant, so nothing about placement could change it) and a
		// target that is either local (an offset in this object's own layout) or external (a name
		// for the same `globals` table the relocations above already used). What no single object
		// could check is a duplicate *across* objects, which is why that check lives here instead
		// of in Linker::resolveInterruptVectors() - each object already checked everything it could
		// see on its own.
		std::vector<InterruptVectorPatch> interruptVectors;
		std::unordered_map<u8, std::string> interruptBoundBy;

		for (usize i = 0; i < members.size(); ++i)
		{
			for (const ObjectInterruptBinding& binding : members[i]->object.interruptBindings)
			{
				// The assembler already refuses these when it can see the whole binding; re-checked
				// here because a hand-built or corrupted .cobj might not have come from it at all.
				if (binding.interruptNumber == 0 || binding.interruptNumber > 63)
				{
					reportError(std::format("{}: interrupt numbers range from 1 to 63, but {} was given",
						members[i]->name, binding.interruptNumber));
					continue;
				}

				u32 target = 0;
				if (binding.isExternal())
				{
					const auto found = globals.find(binding.symbol);
					if (found == globals.end())
					{
						reportError(std::format("Undefined symbol '{}', wanted by '{}'", binding.symbol, members[i]->name));
						continue;
					}
					target = found->second;
				}
				else
				{
					target = baseOf(placements[i], binding.section) + binding.offset;
				}

				if (const auto existing = interruptBoundBy.find(binding.interruptNumber); existing != interruptBoundBy.end())
				{
					reportError(std::format("interrupt {} is already bound to '{}'; second binding to '{}' in '{}'",
						binding.interruptNumber, existing->second, binding.symbol, members[i]->name));
					continue;
				}

				interruptBoundBy.emplace(binding.interruptNumber, binding.symbol);
				interruptVectors.push_back(InterruptVectorPatch{ binding.interruptNumber, target });
			}
		}

		// --- Where it starts ---------------------------------------------------------------------
		u32 entryPoint = 0;
		const auto main = globals.find(std::string(SymbolTable::EntryPointLabelName));
		if (main != globals.end())
		{
			entryPoint = main->second;
		}
		else if (options.requireEntryPoint)
		{
			reportError(std::format("No entry point: none of these objects defines a global '{}'",
				SymbolTable::EntryPointLabelName));
		}

		if (hasErrors())
			return std::nullopt;

		std::vector<u8> debugSection;
		if (options.emitDebugInfo)
		{
			for (usize i = 0; i < members.size(); ++i)
			{
				const ObjectFile& object = members[i]->object;
				if (object.debugSection.empty())
					continue;

				auto piece = DebugInfo::deserialize(object.debugSection);
				if (!piece)
				{
					reportError(std::format("{}: {}", members[i]->name, piece.error()));
					continue;
				}

				_debugInfo.merge(piece.value(), placements[i].text, placements[i].rodata,
					placements[i].data, placements[i].bss);
			}

			if (hasErrors())
				return std::nullopt;

			debugSection = _debugInfo.serialize();
		}

		ProgramHeader header{
			.magic = ProgramHeader::MagicNumber,
			.version = ProgramHeader::CurrentVersion,
			.entryPoint = entryPoint,
			.textSize = textSize,
			.rodataSize = rodataSize,
			.dataSize = dataSize,
			.bssSize = bssSize,
			.minimumStack = 1024
		};

		return Program::make(header, text, rodata, data, interruptVectors, debugSection);
	}

	namespace
	{
		// Whether the instruction ending a piece of code never lets it run on into the next: a jump or a return.
		// Anything else - a conditional branch, a call, data kept between functions - may.
		bool endsControl(const std::vector<u8>& text, u32 begin, u32 end)
		{
			// Zero words (padding) are skipped: the instruction before them is the one that decides.
			while (end >= begin + Instruction::Size)
			{
				const u32 at = end - Instruction::Size;
				const Instruction::RawType raw = static_cast<Instruction::RawType>(text[at]) |
					(static_cast<Instruction::RawType>(text[at + 1]) << 8) |
					(static_cast<Instruction::RawType>(text[at + 2]) << 16) |
					(static_cast<Instruction::RawType>(text[at + 3]) << 24);
				if (raw == 0)
				{
					end = at;
					continue;
				}
				const Opcode opcode = Instruction(raw).opcode();
				return opcode == Opcode::JP || opcode == Opcode::JPR || opcode == Opcode::RET || opcode == Opcode::IRET;
			}
			return false;
		}
	}

	void ObjectLinker::collectUnusedCode(std::vector<ObjectArchive::Member*>& members)
	{
		// Each object's .text in pieces, cut at its global names.
		struct Pieces
		{
			std::vector<u32> starts;          // sorted; the first is 0
			std::vector<bool> live;
			std::vector<usize> relocationOrder; // this object's .text relocations, by offset
			bool movable = false;
		};
		std::vector<Pieces> pieces(members.size());
		std::unordered_map<std::string, std::pair<usize, const ObjectSymbol*>> byName;
		bool defined_twice = false;
		for (usize m = 0; m < members.size(); ++m)
		{
			ObjectFile& object = members[m]->object;
			Pieces& p = pieces[m];
			p.movable = (object.flags & ObjectFile::FlagCompleteTextRelocations) != 0;
			p.starts.push_back(0);
			for (const ObjectSymbol& symbol : object.symbols)
			{
				defined_twice |= !byName.emplace(symbol.name, std::pair{ m, &symbol }).second;
				if (p.movable && symbol.section == SectionType::Text && symbol.offset < object.text.size())
					p.starts.push_back(symbol.offset);
			}
			std::ranges::sort(p.starts);
			p.starts.erase(std::unique(p.starts.begin(), p.starts.end()), p.starts.end());
			p.live.assign(p.starts.size(), false);
			for (usize r = 0; r < object.relocations.size(); ++r)
				if (object.relocations[r].patchedSection == SectionType::Text)
					p.relocationOrder.push_back(r);
			std::ranges::sort(p.relocationOrder, [&](usize a, usize b) { return object.relocations[a].offset < object.relocations[b].offset; });
		}
		// A name defined twice is an error the link reports once every name is in place: collecting first could
		// drop one of the two and hide it.
		if (defined_twice)
			return;

		const auto pieceOf = [&](usize m, u32 offset) -> usize
		{
			const std::vector<u32>& starts = pieces[m].starts;
			const auto after = std::upper_bound(starts.begin(), starts.end(), offset);
			return static_cast<usize>(after - starts.begin()) - 1;
		};
		const auto pieceEnd = [&](usize m, usize k) -> u32
		{
			return k + 1 < pieces[m].starts.size() ? pieces[m].starts[k + 1] : static_cast<u32>(members[m]->object.text.size());
		};

		std::vector<std::pair<usize, usize>> work;
		const auto mark = [&](usize m, SectionType section, u32 offset)
		{
			if (section != SectionType::Text)
				return;
			const usize k = pieceOf(m, offset);
			if (!pieces[m].live[k])
			{
				pieces[m].live[k] = true;
				work.emplace_back(m, k);
			}
		};
		const auto markName = [&](const std::string& name)
		{
			const auto found = byName.find(name);
			if (found != byName.end())
				mark(found->second.first, found->second.second->section, found->second.second->offset);
		};
		const auto markTarget = [&](usize m, const Relocation& relocation)
		{
			if (relocation.isExternal())
				markName(relocation.symbol);
			else
				mark(m, relocation.section, static_cast<u32>(relocation.addend));
		};

		// The roots: where the program starts, what the interrupts run, what the data points at - and all of an
		// object that cannot be cut. With no entry point this is a library, and every global name is what it is for.
		if (byName.contains(std::string(SymbolTable::EntryPointLabelName)))
			markName(std::string(SymbolTable::EntryPointLabelName));
		else
			for (const auto& [name, where] : byName)
				mark(where.first, where.second->section, where.second->offset);
		for (usize m = 0; m < members.size(); ++m)
		{
			const ObjectFile& object = members[m]->object;
			for (const ObjectInterruptBinding& binding : object.interruptBindings)
			{
				if (binding.isExternal())
					markName(binding.symbol);
				else
					mark(m, binding.section, binding.offset);
			}
			for (const Relocation& relocation : object.relocations)
				if (relocation.patchedSection != SectionType::Text)
					markTarget(m, relocation);
			if (!pieces[m].movable)
				for (usize k = 0; k < pieces[m].starts.size(); ++k)
					mark(m, SectionType::Text, pieces[m].starts[k]);
		}

		while (!work.empty())
		{
			const auto [m, k] = work.back();
			work.pop_back();
			const ObjectFile& object = members[m]->object;
			const u32 begin = pieces[m].starts[k];
			const u32 end = pieceEnd(m, k);
			const std::vector<usize>& order = pieces[m].relocationOrder;
			auto from = std::lower_bound(order.begin(), order.end(), begin,
				[&](usize r, u32 value) { return object.relocations[r].offset < value; });
			for (; from != order.end() && object.relocations[*from].offset < end; ++from)
				markTarget(m, object.relocations[*from]);
			if (!endsControl(object.text, begin, end))                  // it may run on into the next piece
			{
				if (k + 1 < pieces[m].starts.size())
					mark(m, SectionType::Text, pieces[m].starts[k + 1]);
				else                                                    // the next object's, laid out right after
					for (usize next = m + 1; next < members.size(); ++next)
						if (!members[next]->object.text.empty())
						{
							mark(next, SectionType::Text, 0);
							break;
						}
			}
		}

		// Each movable object keeps its live pieces, closed up, and everything that pointed into them moves too.
		for (usize m = 0; m < members.size(); ++m)
		{
			Pieces& p = pieces[m];
			ObjectFile& object = members[m]->object;
			if (!p.movable || std::ranges::all_of(p.live, [](bool live) { return live; }))
				continue;
			const u32 oldSize = static_cast<u32>(object.text.size());
			std::vector<i64> shift(p.starts.size(), 0);
			std::vector<u8> text;
			for (usize k = 0; k < p.starts.size(); ++k)
			{
				if (!p.live[k])
					continue;
				const u32 begin = p.starts[k];
				const u32 end = pieceEnd(m, k);
				shift[k] = static_cast<i64>(text.size()) - static_cast<i64>(begin);
				text.insert(text.end(), object.text.begin() + begin, object.text.begin() + end);
			}
			const auto moved = [&](u32 offset) -> u32
			{
				if (offset >= oldSize)
					return static_cast<u32>(text.size()) + (offset - oldSize);   // just past the end stays just past it
				return static_cast<u32>(static_cast<i64>(offset) + shift[pieceOf(m, offset)]);
			};
			const auto kept = [&](u32 offset) { return offset >= oldSize || p.live[pieceOf(m, offset)]; };

			std::erase_if(object.symbols, [&](const ObjectSymbol& symbol)
				{ return symbol.section == SectionType::Text && !kept(symbol.offset); });
			for (ObjectSymbol& symbol : object.symbols)
				if (symbol.section == SectionType::Text)
					symbol.offset = moved(symbol.offset);

			std::erase_if(object.relocations, [&](const Relocation& relocation)
				{ return relocation.patchedSection == SectionType::Text && !kept(relocation.offset); });
			for (Relocation& relocation : object.relocations)
			{
				if (relocation.patchedSection == SectionType::Text)
					relocation.offset = moved(relocation.offset);
				if (!relocation.isExternal() && relocation.section == SectionType::Text)
					relocation.addend = static_cast<i32>(moved(static_cast<u32>(relocation.addend)));
			}
			for (ObjectInterruptBinding& binding : object.interruptBindings)
				if (!binding.isExternal() && binding.section == SectionType::Text)
					binding.offset = moved(binding.offset);

			_bytesCollected += oldSize - static_cast<u32>(text.size());
			object.text = std::move(text);
		}
	}
}

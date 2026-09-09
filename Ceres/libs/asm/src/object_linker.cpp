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
		for (usize i = 0; i < inputs.size(); ++i)
		{
			if (selected[i])
				members.push_back(&inputs[i]);
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

		// --- Filling in the blanks ---------------------------------------------------------------
		std::unordered_set<std::string> reported;
		for (usize i = 0; i < members.size(); ++i)
		{
			const ObjectFile& object = members[i]->object;
			const Placement& placement = placements[i];
			const u32 objectTextStart = placement.text - textStart; // Into the merged buffer

			for (const Relocation& relocation : object.relocations)
			{
				const usize position = static_cast<usize>(objectTextStart) + relocation.offset;
				if (position + Instruction::Size > text.size())
				{
					reportError(std::format("{}: a relocation points past the end of its own code",
						members[i]->name));
					continue;
				}

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
				}

				const Instruction::RawType raw = instruction.raw();
				text[position] = static_cast<u8>(raw & 0xFF);
				text[position + 1] = static_cast<u8>((raw >> 8) & 0xFF);
				text[position + 2] = static_cast<u8>((raw >> 16) & 0xFF);
				text[position + 3] = static_cast<u8>((raw >> 24) & 0xFF);
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

		return Program::make(header, text, rodata, data, debugSection);
	}
}

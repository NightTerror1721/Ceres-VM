#include "debug_info.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <stdexcept>

namespace ceres::debug
{
	namespace
	{
		// Written byte by byte in little-endian rather than by copying the structs, so a .cres
		// carrying debug information means the same thing whatever the host's byte order is —
		// the same rule the instruction encoder already follows.
		void writeU8(std::vector<u8>& out, u8 value) { out.push_back(value); }

		void writeU16(std::vector<u8>& out, u16 value)
		{
			out.push_back(static_cast<u8>(value & 0xFF));
			out.push_back(static_cast<u8>((value >> 8) & 0xFF));
		}

		void writeU32(std::vector<u8>& out, u32 value)
		{
			out.push_back(static_cast<u8>(value & 0xFF));
			out.push_back(static_cast<u8>((value >> 8) & 0xFF));
			out.push_back(static_cast<u8>((value >> 16) & 0xFF));
			out.push_back(static_cast<u8>((value >> 24) & 0xFF));
		}

		struct Reader
		{
			std::span<const u8> bytes;
			usize offset = 0;

			bool has(usize count) const noexcept { return offset + count <= bytes.size(); }

			u8 readU8() noexcept { return bytes[offset++]; }

			u16 readU16() noexcept
			{
				const u16 value = static_cast<u16>(bytes[offset]) |
					(static_cast<u16>(bytes[offset + 1]) << 8);
				offset += 2;
				return value;
			}

			u32 readU32() noexcept
			{
				const u32 value = static_cast<u32>(bytes[offset]) |
					(static_cast<u32>(bytes[offset + 1]) << 8) |
					(static_cast<u32>(bytes[offset + 2]) << 16) |
					(static_cast<u32>(bytes[offset + 3]) << 24);
				offset += 4;
				return value;
			}
		};

		constexpr usize LineEntrySize = 28;   // 6 * u32 + 2 * u16
		constexpr usize SymbolEntrySize = 24; // 5 * u32 + 4 * u8
		constexpr usize FrameEntrySize = 12;  // 2 * u32 + 2 * u16

		// Minimal escaper for the shapes of text that actually appear here (paths and identifiers);
		// not a general JSON serialiser. Mirrors the one main.cpp already uses for diagnostics.
		std::string jsonEscape(std::string_view text)
		{
			std::string out;
			out.reserve(text.size());
			for (char c : text)
			{
				switch (c)
				{
					case '"': out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					default:
						if (static_cast<unsigned char>(c) < 0x20)
							out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
						else
							out += c;
				}
			}
			return out;
		}

		std::string_view scalarTypeName(u8 scalarType) noexcept
		{
			switch (static_cast<ScalarType>(scalarType))
			{
				case ScalarType::U8:  return "u8";
				case ScalarType::U16: return "u16";
				case ScalarType::U32: return "u32";
				case ScalarType::I8:  return "i8";
				case ScalarType::I16: return "i16";
				case ScalarType::I32: return "i32";
				case ScalarType::F32: return "f32";
				default:              return "";
			}
		}

		std::string_view symbolKindName(u8 kind) noexcept
		{
			switch (static_cast<SymbolKind>(kind))
			{
				case SymbolKind::Label:    return "label";
				case SymbolKind::Constant: return "constant";
				case SymbolKind::Variable: return "variable";
				default:                   return "unknown";
			}
		}

		std::string_view sectionName(u8 section) noexcept
		{
			switch (static_cast<SymbolSection>(section))
			{
				case SymbolSection::Text:   return "text";
				case SymbolSection::Rodata: return "rodata";
				case SymbolSection::Data:   return "data";
				case SymbolSection::BSS:    return "bss";
				default:                    return "none";
			}
		}
	}

	// --- DebugInfo: lookups ---------------------------------------------------------------------

	std::string_view DebugInfo::stringAt(u32 offset) const noexcept
	{
		if (offset >= _strings.size())
			return {};

		const usize end = _strings.find('\0', offset);
		return std::string_view(_strings).substr(offset, (end == std::string::npos ? _strings.size() : end) - offset);
	}

	std::string_view DebugInfo::fileName(u32 fileId) const noexcept
	{
		if (fileId >= _fileOffsets.size())
			return {};
		return stringAt(_fileOffsets[fileId]);
	}

	std::optional<SourceLocation> DebugInfo::locationOf(u32 address) const noexcept
	{
		if (_lines.empty())
			return std::nullopt;

		const auto it = std::ranges::lower_bound(_lines, address, {}, &LineEntry::address);

		const LineEntry* entry = nullptr;
		if (it != _lines.end() && it->address == address)
		{
			entry = &*it;
		}
		else if (it != _lines.begin())
		{
			// Not on an instruction boundary. Resolve to the instruction it falls inside, but only
			// that far: anything beyond one word is past the end of .text, not inside it.
			const LineEntry& previous = *std::prev(it);
			if (address - previous.address < AddressTolerance)
				entry = &previous;
		}

		if (entry == nullptr)
			return std::nullopt;

		return SourceLocation{
			.file = fileName(entry->fileId),
			.line = entry->line,
			.column = entry->column,
			.expansionFile = fileName(entry->expansionFileId),
			.expansionLine = entry->expansionLine,
			.flags = entry->flags,
			.macroDepth = entry->macroDepth
		};
	}

	std::optional<u32> DebugInfo::resolveFileId(std::string_view path) const
	{
		for (usize i = 0; i < _fileOffsets.size(); ++i)
		{
			if (fileName(static_cast<u32>(i)) == path)
				return static_cast<u32>(i);
		}

		const std::filesystem::path wanted = std::filesystem::path(path).lexically_normal();
		for (usize i = 0; i < _fileOffsets.size(); ++i)
		{
			if (std::filesystem::path(fileName(static_cast<u32>(i))).lexically_normal() == wanted)
				return static_cast<u32>(i);
		}

		// An editor sends an absolute path; the assembler may have been given a relative one from a
		// different working directory, so neither form above matches. Falling back to the file name
		// recovers that case, but only when exactly one recorded file bears it — otherwise the
		// answer would be a guess between two same-named files in different directories.
		const std::filesystem::path wantedName = wanted.filename();
		if (wantedName.empty())
			return std::nullopt;

		std::optional<u32> onlyMatch;
		for (usize i = 0; i < _fileOffsets.size(); ++i)
		{
			if (std::filesystem::path(fileName(static_cast<u32>(i))).filename() != wantedName)
				continue;

			if (onlyMatch.has_value())
				return std::nullopt; // Ambiguous
			onlyMatch = static_cast<u32>(i);
		}
		return onlyMatch;
	}

	std::vector<u32> DebugInfo::addressesOf(std::string_view file, u32 line) const
	{
		std::vector<u32> out;

		const auto fileId = resolveFileId(file);
		if (!fileId.has_value())
			return out;

		// Matched against the *expansion* site, not where the instruction was written: a line the
		// user can put a breakpoint on is a line in their own file, even when the instruction it
		// produced lives in some macro's body.
		for (const LineEntry& entry : _lines)
		{
			if (entry.expansionFileId == fileId.value() && entry.expansionLine == line)
				out.push_back(entry.address);
		}

		return out;
	}

	std::optional<u32> DebugInfo::firstAddressOfLine(std::string_view file, u32 line) const
	{
		const auto fileId = resolveFileId(file);
		if (!fileId.has_value())
			return std::nullopt;

		for (const LineEntry& entry : _lines)
		{
			if (entry.expansionFileId == fileId.value() &&
				entry.expansionLine == line &&
				(entry.flags & LineFlag::FirstOfLine) != 0)
			{
				return entry.address;
			}
		}

		return std::nullopt;
	}

	const SymbolEntry* DebugInfo::symbolContaining(u32 address) const noexcept
	{
		const SymbolEntry* best = nullptr;
		for (const SymbolEntry& symbol : _symbols)
		{
			if (symbol.size == 0 || symbol.address > address)
				continue;
			if (address - symbol.address >= symbol.size)
				continue;
			if (best == nullptr || symbol.address > best->address)
				best = &symbol;
		}
		return best;
	}

	const SymbolEntry* DebugInfo::symbolNamed(std::string_view name) const noexcept
	{
		for (const SymbolEntry& symbol : _symbols)
		{
			if (symbolName(symbol) == name)
				return &symbol;
		}
		return nullptr;
	}

	// --- DebugInfo: serialisation ---------------------------------------------------------------

	const FrameEntry* DebugInfo::frameAt(u32 address) const noexcept
	{
		// Few enough per program that a scan is cheaper than anything cleverer, and they do
		// not overlap: a function ends where the next one begins.
		for (const FrameEntry& frame : _frames)
		{
			if (address >= frame.address && address < frame.endAddress)
				return &frame;
		}
		return nullptr;
	}

	std::vector<u8> DebugInfo::serialize() const
	{
		const u32 fileCount = static_cast<u32>(_fileOffsets.size());
		const u32 lineCount = static_cast<u32>(_lines.size());
		const u32 symbolCount = static_cast<u32>(_symbols.size());
		const u32 frameCount = static_cast<u32>(_frames.size());
		const u32 stringsSize = static_cast<u32>(_strings.size());
		const u32 totalSize = static_cast<u32>(
			HeaderSize +
			fileCount * sizeof(u32) +
			lineCount * LineEntrySize +
			symbolCount * SymbolEntrySize +
			frameCount * FrameEntrySize +
			stringsSize);

		std::vector<u8> out;
		out.reserve(totalSize);

		writeU32(out, MagicNumber);
		writeU16(out, CurrentVersion);
		writeU16(out, 0); // reserved
		writeU32(out, totalSize);
		writeU32(out, fileCount);
		writeU32(out, lineCount);
		writeU32(out, symbolCount);
		writeU32(out, stringsSize);
		writeU32(out, frameCount);

		for (u32 offset : _fileOffsets)
			writeU32(out, offset);

		for (const LineEntry& entry : _lines)
		{
			writeU32(out, entry.address);
			writeU32(out, entry.fileId);
			writeU32(out, entry.line);
			writeU32(out, entry.column);
			writeU32(out, entry.expansionFileId);
			writeU32(out, entry.expansionLine);
			writeU16(out, entry.flags);
			writeU16(out, entry.macroDepth);
		}

		for (const SymbolEntry& symbol : _symbols)
		{
			writeU32(out, symbol.nameOffset);
			writeU32(out, symbol.address);
			writeU32(out, symbol.size);
			writeU32(out, symbol.elementCount);
			writeU32(out, symbol.value);
			writeU8(out, symbol.kind);
			writeU8(out, symbol.section);
			writeU8(out, symbol.scalarType);
			writeU8(out, symbol.flags);
		}

		for (const FrameEntry& frame : _frames)
		{
			writeU32(out, frame.address);
			writeU32(out, frame.endAddress);
			writeU16(out, frame.frameSize);
			writeU16(out, frame.flags);
		}

		out.insert(out.end(), _strings.begin(), _strings.end());

		return out;
	}

	std::expected<DebugInfo, std::string> DebugInfo::deserialize(std::span<const u8> bytes)
	{
		Reader reader{ bytes };

		if (!reader.has(HeaderSize))
			return std::unexpected("Debug section is too small to contain a header");

		const u32 magic = reader.readU32();
		if (magic != MagicNumber)
			return std::unexpected("Invalid magic number in debug section");

		const u16 version = reader.readU16();
		if (version == 0 || version > CurrentVersion)
			return std::unexpected("Unsupported debug section version " + std::to_string(version));

		reader.readU16(); // reserved
		const u32 totalSize = reader.readU32();
		const u32 fileCount = reader.readU32();
		const u32 lineCount = reader.readU32();
		const u32 symbolCount = reader.readU32();
		const u32 stringsSize = reader.readU32();
		// Version 1 wrote a reserved zero here, which reads back as "no frames".
		const u32 frameCount = reader.readU32();

		if (totalSize > bytes.size())
			return std::unexpected("Debug section declares more bytes than are present");

		const usize declared =
			HeaderSize +
			static_cast<usize>(fileCount) * sizeof(u32) +
			static_cast<usize>(lineCount) * LineEntrySize +
			static_cast<usize>(symbolCount) * SymbolEntrySize +
			static_cast<usize>(frameCount) * FrameEntrySize +
			stringsSize;

		if (declared != totalSize)
			return std::unexpected("Debug section table sizes do not add up to its declared length");

		std::vector<u32> fileOffsets;
		fileOffsets.reserve(fileCount);
		for (u32 i = 0; i < fileCount; ++i)
		{
			if (!reader.has(sizeof(u32)))
				return std::unexpected("Debug section ended inside the file table");
			fileOffsets.push_back(reader.readU32());
		}

		DebugInfo info;
		info._lines.reserve(lineCount);
		for (u32 i = 0; i < lineCount; ++i)
		{
			if (!reader.has(LineEntrySize))
				return std::unexpected("Debug section ended inside the line table");

			LineEntry entry;
			entry.address = reader.readU32();
			entry.fileId = reader.readU32();
			entry.line = reader.readU32();
			entry.column = reader.readU32();
			entry.expansionFileId = reader.readU32();
			entry.expansionLine = reader.readU32();
			entry.flags = reader.readU16();
			entry.macroDepth = reader.readU16();
			info._lines.push_back(entry);
		}

		info._symbols.reserve(symbolCount);
		for (u32 i = 0; i < symbolCount; ++i)
		{
			if (!reader.has(SymbolEntrySize))
				return std::unexpected("Debug section ended inside the symbol table");

			SymbolEntry symbol;
			symbol.nameOffset = reader.readU32();
			symbol.address = reader.readU32();
			symbol.size = reader.readU32();
			symbol.elementCount = reader.readU32();
			symbol.value = reader.readU32();
			symbol.kind = reader.readU8();
			symbol.section = reader.readU8();
			symbol.scalarType = reader.readU8();
			symbol.flags = reader.readU8();
			info._symbols.push_back(symbol);
		}

		info._frames.reserve(frameCount);
		for (u32 i = 0; i < frameCount; ++i)
		{
			if (!reader.has(FrameEntrySize))
				return std::unexpected("Debug section ended inside the frame table");

			FrameEntry frame;
			frame.address = reader.readU32();
			frame.endAddress = reader.readU32();
			frame.frameSize = reader.readU16();
			frame.flags = reader.readU16();
			info._frames.push_back(frame);
		}

		if (!reader.has(stringsSize))
			return std::unexpected("Debug section ended inside the string table");

		info._strings.assign(
			reinterpret_cast<const char*>(bytes.data() + reader.offset),
			static_cast<usize>(stringsSize));

		for (u32 offset : fileOffsets)
		{
			if (offset > stringsSize)
				return std::unexpected("Debug section file table points outside the string table");
		}
		info._fileOffsets = std::move(fileOffsets);

		return info;
	}

	std::string DebugInfo::toJson() const
	{
		std::string out = "{\"files\":[";

		for (usize i = 0; i < _fileOffsets.size(); ++i)
		{
			if (i != 0)
				out += ',';
			out += std::format("\"{}\"", jsonEscape(fileName(static_cast<u32>(i))));
		}

		out += "],\"lines\":[";
		for (usize i = 0; i < _lines.size(); ++i)
		{
			const LineEntry& entry = _lines[i];
			if (i != 0)
				out += ',';
			out += std::format(
				"{{\"address\":{},\"file\":\"{}\",\"line\":{},\"expansionFile\":\"{}\",\"expansionLine\":{}"
				",\"firstOfLine\":{},\"macroExpansion\":{},\"padding\":{},\"macroDepth\":{}}}",
				entry.address,
				jsonEscape(fileName(entry.fileId)),
				entry.line,
				jsonEscape(fileName(entry.expansionFileId)),
				entry.expansionLine,
				(entry.flags & LineFlag::FirstOfLine) != 0 ? "true" : "false",
				(entry.flags & LineFlag::MacroExpansion) != 0 ? "true" : "false",
				(entry.flags & LineFlag::PseudoPadding) != 0 ? "true" : "false",
				entry.macroDepth);
		}

		out += "],\"symbols\":[";
		for (usize i = 0; i < _symbols.size(); ++i)
		{
			const SymbolEntry& symbol = _symbols[i];
			if (i != 0)
				out += ',';
			out += std::format(
				"{{\"name\":\"{}\",\"kind\":\"{}\",\"section\":\"{}\",\"address\":{},\"size\":{}"
				",\"type\":\"{}\",\"elements\":{},\"global\":{},\"readonly\":{}",
				jsonEscape(symbolName(symbol)),
				symbolKindName(symbol.kind),
				sectionName(symbol.section),
				symbol.address,
				symbol.size,
				scalarTypeName(symbol.scalarType),
				symbol.elementCount,
				(symbol.flags & SymbolFlag::Global) != 0 ? "true" : "false",
				(symbol.flags & SymbolFlag::Readonly) != 0 ? "true" : "false");

			if ((symbol.flags & SymbolFlag::HasValue) != 0)
				out += std::format(",\"value\":{}", symbol.value);

			out += '}';
		}

		out += "]}";
		return out;
	}

	// --- DebugInfoBuilder -----------------------------------------------------------------------

	u32 DebugInfoBuilder::internFile(std::string_view path)
	{
		// Deduplication falls out of interning the string first: the same path always yields the
		// same offset, so the search below can compare offsets rather than text.
		const u32 offset = internString(path);

		for (usize i = 0; i < _info._fileOffsets.size(); ++i)
		{
			if (_info._fileOffsets[i] == offset)
				return static_cast<u32>(i);
		}

		_info._fileOffsets.push_back(offset);
		return static_cast<u32>(_info._fileOffsets.size() - 1);
	}

	u32 DebugInfoBuilder::internString(std::string_view text)
	{
		// Linear, but the string blob only ever holds symbol names — a few hundred at most — and
		// this runs once per assembly.
		usize offset = 0;
		while (offset < _info._strings.size())
		{
			const usize end = _info._strings.find('\0', offset);
			const usize length = (end == std::string::npos ? _info._strings.size() : end) - offset;
			if (std::string_view(_info._strings).substr(offset, length) == text)
				return static_cast<u32>(offset);
			if (end == std::string::npos)
				break;
			offset = end + 1;
		}

		const u32 result = static_cast<u32>(_info._strings.size());
		_info._strings.append(text);
		_info._strings.push_back('\0');
		return result;
	}

	void DebugInfoBuilder::addLine(const LineEntry& entry)
	{
		_info._lines.push_back(entry);
	}

	void DebugInfoBuilder::addSymbol(const SymbolEntry& entry)
	{
		_info._symbols.push_back(entry);
	}

	void DebugInfoBuilder::addFrame(const FrameEntry& entry)
	{
		_info._frames.push_back(entry);
	}

	DebugInfo DebugInfoBuilder::release()
	{
		if (_released)
			throw std::logic_error("Debug info has already been released");

		_released = true;

		std::ranges::stable_sort(_info._lines, {}, &LineEntry::address);

		// By name as well as address, because symbols arrive from an unordered_map whose iteration
		// order is unspecified and can differ between runs. Sorting on the address alone would
		// leave same-addressed symbols - a label and the first variable after it - in whatever
		// order the hash table happened to produce, and the emitted bytes would stop being
		// reproducible.
		std::ranges::stable_sort(_info._symbols,
			[&info = _info](const SymbolEntry& a, const SymbolEntry& b)
			{
				if (a.address != b.address)
					return a.address < b.address;
				return info.stringAt(a.nameOffset) < info.stringAt(b.nameOffset);
			});

		// A source line owns a run of consecutive addresses; the first of them is where execution
		// enters it, and so where a line breakpoint belongs. Keyed on the expansion site rather
		// than on where the instruction was written, so two calls to the same macro are two runs
		// rather than one, and a `la` and its padding are one run rather than three lines.
		u32 previousFileId = 0;
		u32 previousLine = 0;
		bool first = true;
		for (LineEntry& entry : _info._lines)
		{
			entry.flags &= static_cast<u16>(~LineFlag::FirstOfLine);

			if (first || entry.expansionFileId != previousFileId || entry.expansionLine != previousLine)
				entry.flags |= LineFlag::FirstOfLine;

			previousFileId = entry.expansionFileId;
			previousLine = entry.expansionLine;
			first = false;
		}

		return std::move(_info);
	}
}

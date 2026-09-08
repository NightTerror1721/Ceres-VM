#pragma once

// The correspondence between source text and machine addresses, which the assembler computes in
// full and — until this existed — threw away the moment assemble() returned. Every address here is
// absolute and final: the linker resolves symbols at assembly time and the loader always places
// the sections at the same offsets, so there is no relocation step to account for.
//
// This is deliberately its own layer rather than part of the assembler: the consumers are the
// debugger, the listing, and editor tooling, none of which should have to link the parser to ask
// where line 42 ended up.

#include "common/types.h"
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ceres::debug
{
	// Mirrors casm::DataTypeScalarCode, but as its own enumeration: this one is written to disk, so
	// it has to keep its numbering even if the assembler's internal one is ever reordered.
	enum class ScalarType : u8
	{
		Invalid = 0,
		U8 = 1,
		U16 = 2,
		U32 = 3,
		I8 = 4,
		I16 = 5,
		I32 = 6,
		F32 = 7,
	};

	enum class SymbolKind : u8
	{
		Label = 0,
		Constant = 1,
		Variable = 2,
	};

	enum class SymbolSection : u8
	{
		Text = 0,
		Rodata = 1,
		Data = 2,
		BSS = 3,
		None = 4, // A constant occupies no memory, so it belongs to no section.
	};

	namespace LineFlag
	{
		inline constexpr u16 None = 0;
		// The address a line breakpoint should be placed at. A source line can occupy several
		// addresses — a pseudo-instruction expands to two or three words, a macro call to many —
		// and only the first of them is where execution enters the line.
		inline constexpr u16 FirstOfLine = 1 << 0;
		// The word came from a macro body rather than from code written at this spot.
		inline constexpr u16 MacroExpansion = 1 << 1;
		// A NOP the emitter wrote to fill out the size reserved for a pseudo-instruction whose
		// chosen overload turned out shorter. Stepping should pass straight through these.
		inline constexpr u16 PseudoPadding = 1 << 2;
	}

	namespace SymbolFlag
	{
		inline constexpr u8 None = 0;
		inline constexpr u8 Global = 1 << 0;
		inline constexpr u8 Readonly = 1 << 1;
		inline constexpr u8 HasValue = 1 << 2; // `value` below holds a constant's scalar bits
	}

	struct LineEntry
	{
		u32 address = 0;
		u32 fileId = 0;          // Where the instruction was *written* (a macro body, for one expanded from a macro)
		u32 line = 0;
		u32 column = 0;          // Reserved: statements only carry line numbers today
		u32 expansionFileId = 0; // Where the user's own code is — equal to fileId outside a macro
		u32 expansionLine = 0;
		u16 flags = LineFlag::None;
		u16 macroDepth = 0;      // 0 for code written by hand
	};

	// What a function does to the stack on the way in. The call stack is otherwise reconstructed
	// by watching CALL and RET go past, which a program that unwinds by hand or jumps into the
	// middle of a subroutine can desynchronise. With this, a frame that has a frame pointer can
	// be walked exactly instead of guessed at.
	struct FrameEntry
	{
		u32 address = 0;    // Where the function starts
		u32 endAddress = 0; // One past its last instruction
		u16 frameSize = 0;  // The operand of its `enter`, or zero if it has none
		u16 flags = 0;      // bit 0: opens a frame pointer with `enter`
	};

	namespace FrameFlag
	{
		inline constexpr u16 None = 0;
		inline constexpr u16 HasFramePointer = 1u << 0;
	}

	struct SymbolEntry
	{
		u32 nameOffset = 0;   // Into the string blob
		u32 address = 0;      // 0 for a constant, which has no address
		u32 size = 0;         // Bytes occupied; 0 for labels and constants
		u32 elementCount = 1; // 1 scalar, N array, 0 unsized array
		u32 value = 0;        // Raw bits of a constant's scalar value; only when SymbolFlag::HasValue
		u8 kind = static_cast<u8>(SymbolKind::Label);
		u8 section = static_cast<u8>(SymbolSection::Text);
		u8 scalarType = static_cast<u8>(ScalarType::Invalid);
		u8 flags = SymbolFlag::None;
	};

	struct SourceLocation
	{
		std::string_view file;          // Where it was written
		u32 line = 0;
		u32 column = 0;
		std::string_view expansionFile; // Where the user's code is
		u32 expansionLine = 0;
		u16 flags = LineFlag::None;
		u16 macroDepth = 0;

		constexpr bool isMacroExpansion() const noexcept { return (flags & LineFlag::MacroExpansion) != 0; }
		constexpr bool isPadding() const noexcept { return (flags & LineFlag::PseudoPadding) != 0; }
	};

	class DebugInfoBuilder;

	class DebugInfo
	{
	public:
		// 'CDBG' in ASCII, by the same convention as ProgramHeader's 'CRES'.
		static inline constexpr u32 MagicNumber = 0x43444247;
		// 2 added the frame table. A version 1 section still reads: its frame count is simply zero,
		// because the field it would have lived in was the reserved word.
		static inline constexpr u16 CurrentVersion = 2;
		// magic, version, reserved, totalSize, fileCount, lineCount, symbolCount, stringsSize, frameCount
		static inline constexpr usize HeaderSize = 32;

		// Every instruction word gets its own entry, so an address that is not an exact match can
		// still be resolved to the instruction containing it — but only within one word, or a
		// stray address past the end of .text would silently resolve to the last instruction.
		static inline constexpr u32 AddressTolerance = 4;

	private:
		// Paths live in the same string blob as symbol names, and the file table is offsets into
		// it. Keeping them there rather than as separate strings is what makes serialisation
		// idempotent: a round trip has nothing left to append.
		std::vector<u32> _fileOffsets;
		std::vector<LineEntry> _lines;     // Sorted by address
		std::vector<SymbolEntry> _symbols; // Sorted by address
		std::vector<FrameEntry> _frames;   // Sorted by address
		std::string _strings;

	public:
		DebugInfo() = default;
		DebugInfo(const DebugInfo&) = default;
		DebugInfo(DebugInfo&&) = default;
		~DebugInfo() = default;

		DebugInfo& operator=(const DebugInfo&) = default;
		DebugInfo& operator=(DebugInfo&&) = default;

	public:
		bool isEmpty() const noexcept { return _lines.empty() && _symbols.empty(); }

		usize fileCount() const noexcept { return _fileOffsets.size(); }
		std::span<const LineEntry> lines() const noexcept { return _lines; }
		std::span<const SymbolEntry> symbols() const noexcept { return _symbols; }
		std::span<const FrameEntry> frames() const noexcept { return _frames; }

		// The function containing an address, if the assembler recorded one for it.
		const FrameEntry* frameAt(u32 address) const noexcept;

		std::string_view stringAt(u32 offset) const noexcept;
		std::string_view fileName(u32 fileId) const noexcept;
		std::string_view symbolName(const SymbolEntry& symbol) const noexcept { return stringAt(symbol.nameOffset); }

	public:
		// The two queries the whole thing exists for.
		std::optional<SourceLocation> locationOf(u32 address) const noexcept;
		std::vector<u32> addressesOf(std::string_view file, u32 line) const;

		// Where a breakpoint on that line belongs, which is not simply the lowest address: a line
		// occupying several words is entered only at the first of them.
		std::optional<u32> firstAddressOfLine(std::string_view file, u32 line) const;

		const SymbolEntry* symbolContaining(u32 address) const noexcept;
		const SymbolEntry* symbolNamed(std::string_view name) const noexcept;

		// Matches a path given by a caller against the ones recorded at assembly time, which are
		// whatever was written on the command line. Tries the string as-is, then the lexically
		// normalised form, then — only when it is unambiguous — the file name alone, so an editor
		// passing an absolute path still finds a unit assembled from a relative one.
		std::optional<u32> resolveFileId(std::string_view path) const;

	public:
		std::vector<u8> serialize() const;
		static std::expected<DebugInfo, std::string> deserialize(std::span<const u8> bytes);

		std::string toJson() const;

		friend class DebugInfoBuilder;
	};

	// Accumulates entries while the binary is being emitted, then hands over a queryable DebugInfo.
	// Follows the same build-then-release shape as TranslationUnitBuilder.
	class DebugInfoBuilder
	{
	private:
		DebugInfo _info;
		bool _released = false;

	public:
		DebugInfoBuilder() = default;
		DebugInfoBuilder(const DebugInfoBuilder&) = delete;
		DebugInfoBuilder(DebugInfoBuilder&&) = default;
		~DebugInfoBuilder() = default;

		DebugInfoBuilder& operator=(const DebugInfoBuilder&) = delete;
		DebugInfoBuilder& operator=(DebugInfoBuilder&&) = default;

	public:
		u32 internFile(std::string_view path);
		u32 internString(std::string_view text);

		void addLine(const LineEntry& entry);
		void addSymbol(const SymbolEntry& entry);
		void addFrame(const FrameEntry& entry);

		// Sorts both tables by address and marks the first entry of every source line. Marking has
		// to happen here rather than at insertion: what counts as "the first word of this line" is
		// only knowable once every word that line produced is present.
		DebugInfo release();
	};
}

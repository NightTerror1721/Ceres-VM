#pragma once

#include <ceres/core/isa/address.h>
#include <string>
#include <vector>
#include <span>
#include <expected>
#include <filesystem>

namespace ceres::fmt
{
	#pragma pack(push, 1)
	struct ProgramHeader
	{
		static inline constexpr u32 MagicNumber = 0x43524553; // 'CRES' in ASCII
		static inline constexpr u16 CurrentVersion = 4;
		// Three changes so far have altered what existing bytes mean, and none can be detected from
		// the file itself - which is why there is a lower bound at all, not just an upper one.
		//
		//   1 -> 2  The comparison jumps needed sixteen opcodes where eight were free, so everything
		//           above the control-flow block moved. A v1 PUSH (0x70) reads as v2 JAB.
		//   2 -> 3  Memory displacements became signed. A v2 `[r1 + 65528]` was the only way to
		//           write what is now `[r1 - 8]`, so the same sixteen bits mean different addresses.
		//   3 -> 4  The port-based I/O family (0xA0-0xB3: in/out and every variant) was retired in
		//           favour of memory-mapped devices reached through ordinary loads and stores - see
		//           docs/07-IO-Devices-and-Ports.md. A v3 `outb` (0xAD) decodes as an unmapped
		//           opcode now, not silently as something else, but running it was never going to
		//           do what the program asked either way.
		static inline constexpr u16 MinimumSupportedVersion = 4;

		u32 magic;
		u16 version;
		u16 flags;
		u32 entryPoint;

		u32 textSize;
		u32 rodataSize;
		u32 dataSize;
		u32 bssSize;

		u32 minimumStack;
	};
	#pragma pack(pop)

	// Bits of ProgramHeader::flags, which was reserved and unused until debug information needed
	// somewhere to announce itself. Using a flag rather than raising `version` is deliberate: the
	// version check rejects anything newer than it knows, so a `ceres` built before this change
	// would refuse a file it can in fact run perfectly well, whereas a flag it does not recognise
	// simply leaves the trailing bytes unread.
	namespace ProgramFlags
	{
		// A debug section follows the data section: a little-endian u32 byte count, then that many
		// bytes. The count is written here rather than inside the section so that Program can skip
		// or capture it without knowing anything about its contents.
		inline constexpr u16 HasDebugInfo = 1 << 0;
		// An interrupt vector patch table follows .data (and precedes the debug section, if any):
		// a little-endian u32 count, then that many 5-byte entries. See InterruptVectorPatch.
		inline constexpr u16 HasInterruptVectors = 1 << 1;
	}

	// One entry of the vector patch table: overwrite the null-page vector for `interruptNumber`
	// (1-63; 0 is the reset vector and is never patched this way) with `handlerAddress`. Applied by
	// the loader after the BIOS installs its default stub, so an interrupt nothing bound still
	// falls through to that stub exactly as it always has.
	#pragma pack(push, 1)
	struct InterruptVectorPatch
	{
		u8 interruptNumber;
		u32 handlerAddress;
	};
	#pragma pack(pop)

	class Program
	{
	public:
		using ByteType = u8;

	private:
		ProgramHeader _header;
		std::vector<ByteType> _text;
		std::vector<ByteType> _rodata;
		std::vector<ByteType> _data;
		std::vector<InterruptVectorPatch> _interruptVectors;
		// Opaque here on purpose: its layout belongs to ceres::debug, and the VM has no business
		// knowing it. Program only has to carry it from one end of a file to the other.
		std::vector<ByteType> _debugSection;

	public:
		Program() = delete;
		Program(const Program&) = default;
		Program(Program&&) = default;
		~Program() = default;

		Program& operator=(const Program&) = default;
		Program& operator=(Program&&) = default;

	private:
		Program(const ProgramHeader& header, std::vector<ByteType>&& text, std::vector<ByteType>&& rodata, std::vector<ByteType>&& data,
			std::vector<InterruptVectorPatch>&& interruptVectors = {}, std::vector<ByteType>&& debugSection = {}) :
			_header(header),
			_text(std::move(text)),
			_rodata(std::move(rodata)),
			_data(std::move(data)),
			_interruptVectors(std::move(interruptVectors)),
			_debugSection(std::move(debugSection))
		{}

	public:
		const ProgramHeader& header() const noexcept { return _header; }
		std::span<const ByteType> text() const noexcept { return _text; }
		std::span<const ByteType> rodata() const noexcept { return _rodata; }
		std::span<const ByteType> data() const noexcept { return _data; }

		bool hasInterruptVectors() const noexcept { return !_interruptVectors.empty(); }
		std::span<const InterruptVectorPatch> interruptVectors() const noexcept { return _interruptVectors; }

		bool hasDebugSection() const noexcept { return !_debugSection.empty(); }
		std::span<const ByteType> debugSection() const noexcept { return _debugSection; }

	public:
		static Program make(
			const ProgramHeader& header,
			std::span<const ByteType> text,
			std::span<const ByteType> rodata,
			std::span<const ByteType> data,
			std::span<const InterruptVectorPatch> interruptVectors = {},
			std::span<const ByteType> debugSection = {}
		);

		// Program could be loaded five different ways and written none, which is why the assembler
		// and the VM could only ever run in the same process.
		std::expected<void, std::string> saveToFile(const std::filesystem::path& filePath) const;
		std::expected<void, std::string> writeToStream(std::ostream& stream) const;

		std::expected<Program, std::string> static loadFromFile(const std::filesystem::path& filePath);
		std::expected<Program, std::string> static loadFromBytes(std::span<const ByteType> bytes);
		std::expected<Program, std::string> static loadFromStream(std::istream& stream);
		std::expected<Program, std::string> static loadFromMemory(const void* memory, usize size);
		std::expected<Program, std::string> static loadFromString(const std::string& str);
	};
}

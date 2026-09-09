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
		static inline constexpr u16 CurrentVersion = 3;
		// Two changes so far have altered what existing bytes mean, and neither can be detected from
		// the file itself - which is why there is a lower bound at all, not just an upper one.
		//
		//   1 -> 2  The comparison jumps needed sixteen opcodes where eight were free, so everything
		//           above the control-flow block moved. A v1 PUSH (0x70) reads as v2 JAB.
		//   2 -> 3  Memory displacements became signed. A v2 `[r1 + 65528]` was the only way to
		//           write what is now `[r1 - 8]`, so the same sixteen bits mean different addresses.
		static inline constexpr u16 MinimumSupportedVersion = 3;

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
	}

	class Program
	{
	public:
		using ByteType = u8;

	private:
		ProgramHeader _header;
		std::vector<ByteType> _text;
		std::vector<ByteType> _rodata;
		std::vector<ByteType> _data;
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
		Program(const ProgramHeader& header, std::vector<ByteType>&& text, std::vector<ByteType>&& rodata, std::vector<ByteType>&& data, std::vector<ByteType>&& debugSection = {}) :
			_header(header),
			_text(std::move(text)),
			_rodata(std::move(rodata)),
			_data(std::move(data)),
			_debugSection(std::move(debugSection))
		{}

	public:
		const ProgramHeader& header() const noexcept { return _header; }
		std::span<const ByteType> text() const noexcept { return _text; }
		std::span<const ByteType> rodata() const noexcept { return _rodata; }
		std::span<const ByteType> data() const noexcept { return _data; }

		bool hasDebugSection() const noexcept { return !_debugSection.empty(); }
		std::span<const ByteType> debugSection() const noexcept { return _debugSection; }

	public:
		static Program make(
			const ProgramHeader& header,
			std::span<const ByteType> text,
			std::span<const ByteType> rodata,
			std::span<const ByteType> data,
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

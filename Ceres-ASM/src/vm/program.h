#pragma once

#include "address.h"
#include <string>
#include <vector>
#include <span>
#include <expected>
#include <filesystem>

namespace ceres::vm
{
	#pragma pack(push, 1)
	struct ProgramHeader
	{
		static inline constexpr u32 MagicNumber = 0x43524553; // 'CRES' in ASCII
		static inline constexpr u16 CurrentVersion = 2;
		// Version 1 numbered the opcodes differently: everything above the control-flow block moved
		// when the comparison jumps were added. A v1 file is not a v2 file with unknown instructions
		// in it, it is a file where PUSH means JAB - so it has to be rejected rather than run. The
		// check that only looked for versions *newer* than this one would have executed it happily.
		static inline constexpr u16 MinimumSupportedVersion = 2;

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

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
		static inline constexpr u16 CurrentVersion = 1;

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

	class Program
	{
	public:
		using ByteType = u8;

	private:
		ProgramHeader _header;
		std::vector<ByteType> _text;
		std::vector<ByteType> _rodata;
		std::vector<ByteType> _data;

	public:
		Program() = delete;
		Program(const Program&) = default;
		Program(Program&&) = default;
		~Program() = default;

		Program& operator=(const Program&) = default;
		Program& operator=(Program&&) = default;

	private:
		Program(const ProgramHeader& header, std::vector<ByteType>&& text, std::vector<ByteType>&& rodata, std::vector<ByteType>&& data) :
			_header(header),
			_text(std::move(text)),
			_rodata(std::move(rodata)),
			_data(std::move(data))
		{}

	public:
		const ProgramHeader& header() const noexcept { return _header; }
		std::span<const ByteType> text() const noexcept { return _text; }
		std::span<const ByteType> rodata() const noexcept { return _rodata; }
		std::span<const ByteType> data() const noexcept { return _data; }

	public:
		static Program make(
			const ProgramHeader& header,
			std::span<const ByteType> text,
			std::span<const ByteType> rodata,
			std::span<const ByteType> data
		);

		std::expected<Program, std::string> static loadFromFile(const std::filesystem::path& filePath);
		std::expected<Program, std::string> static loadFromBytes(std::span<const ByteType> bytes);
		std::expected<Program, std::string> static loadFromStream(std::istream& stream);
		std::expected<Program, std::string> static loadFromMemory(const void* memory, usize size);
		std::expected<Program, std::string> static loadFromString(const std::string& str);
	};
}

#include <ceres/core/format/program.h>
#include <fstream>

namespace ceres::fmt
{
	namespace
	{
		// Reads the optional trailing debug section. A truncated one is dropped rather than
		// failing the load: nothing needs it to run the program, so a half-copied file should
		// still execute — it just cannot be debugged.
		std::vector<Program::ByteType> readDebugSection(std::istream& stream, const ProgramHeader& header)
		{
			if ((header.flags & ProgramFlags::HasDebugInfo) == 0)
				return {};

			Program::ByteType lengthPrefix[4]{};
			stream.read(reinterpret_cast<char*>(lengthPrefix), sizeof(lengthPrefix));
			if (!stream)
				return {};

			const u32 size =
				static_cast<u32>(lengthPrefix[0]) |
				(static_cast<u32>(lengthPrefix[1]) << 8) |
				(static_cast<u32>(lengthPrefix[2]) << 16) |
				(static_cast<u32>(lengthPrefix[3]) << 24);

			if (size == 0)
				return {};

			std::vector<Program::ByteType> section(size);
			stream.read(reinterpret_cast<char*>(section.data()), size);
			if (!stream)
				return {};

			return section;
		}

		std::vector<Program::ByteType> readDebugSection(std::span<const Program::ByteType> bytes, usize offset, const ProgramHeader& header)
		{
			if ((header.flags & ProgramFlags::HasDebugInfo) == 0)
				return {};

			if (offset + sizeof(u32) > bytes.size())
				return {};

			const u32 size =
				static_cast<u32>(bytes[offset]) |
				(static_cast<u32>(bytes[offset + 1]) << 8) |
				(static_cast<u32>(bytes[offset + 2]) << 16) |
				(static_cast<u32>(bytes[offset + 3]) << 24);

			const usize start = offset + sizeof(u32);
			if (size == 0 || start + size > bytes.size())
				return {};

			return std::vector<Program::ByteType>(bytes.begin() + start, bytes.begin() + start + size);
		}
	}

	Program Program::make(
		const ProgramHeader& header,
		std::span<const ByteType> text,
		std::span<const ByteType> rodata,
		std::span<const ByteType> data,
		std::span<const ByteType> debugSection
	)
	{
		// The flag is derived from what was actually handed over rather than trusted from the
		// caller's header, so the two can never disagree about whether a debug section is there.
		ProgramHeader adjusted = header;
		if (debugSection.empty())
			adjusted.flags &= static_cast<u16>(~ProgramFlags::HasDebugInfo);
		else
			adjusted.flags |= ProgramFlags::HasDebugInfo;

		return Program(
			adjusted,
			std::vector<ByteType>(text.begin(), text.end()),
			std::vector<ByteType>(rodata.begin(), rodata.end()),
			std::vector<ByteType>(data.begin(), data.end()),
			std::vector<ByteType>(debugSection.begin(), debugSection.end())
		);
	}

	std::expected<void, std::string> Program::writeToStream(std::ostream& stream) const
	{
		if (!stream)
			return std::unexpected("Invalid output stream");

		// Header first, then the three sections in the order loadProgram expects them in memory.
		stream.write(reinterpret_cast<const char*>(&_header), sizeof(_header));

		const auto writeSection = [&stream](const std::vector<ByteType>& section)
		{
			if (!section.empty())
				stream.write(reinterpret_cast<const char*>(section.data()), static_cast<std::streamsize>(section.size()));
		};

		writeSection(_text);
		writeSection(_rodata);
		writeSection(_data);

		// Last, and behind a length prefix, so a reader that does not care about debug information
		// never has to look at it and a reader that does never has to guess where it ends.
		if (!_debugSection.empty())
		{
			const u32 size = static_cast<u32>(_debugSection.size());
			const char lengthPrefix[4] = {
				static_cast<char>(size & 0xFF),
				static_cast<char>((size >> 8) & 0xFF),
				static_cast<char>((size >> 16) & 0xFF),
				static_cast<char>((size >> 24) & 0xFF)
			};
			stream.write(lengthPrefix, sizeof(lengthPrefix));
			stream.write(reinterpret_cast<const char*>(_debugSection.data()), static_cast<std::streamsize>(_debugSection.size()));
		}

		if (!stream)
			return std::unexpected("Failed while writing the program");

		return {};
	}

	std::expected<void, std::string> Program::saveToFile(const std::filesystem::path& filePath) const
	{
		std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
		if (!file)
			return std::unexpected("Failed to open file for writing: " + filePath.string());

		return writeToStream(file);
	}

	std::expected<Program, std::string> Program::loadFromFile(const std::filesystem::path& filePath)
	{
		std::ifstream file(filePath, std::ios::binary);
		if (!file)
			return std::unexpected("Failed to open file: " + filePath.string());

		ProgramHeader header;
		file.read(reinterpret_cast<char*>(&header), sizeof(header));
		if (!file)
			return std::unexpected("Failed to read program header from file: " + filePath.string());

		if (header.magic != ProgramHeader::MagicNumber)
			return std::unexpected("Invalid magic number in file: " + filePath.string());

		if (header.version > ProgramHeader::CurrentVersion || header.version < ProgramHeader::MinimumSupportedVersion)
			return std::unexpected("Unsupported .cres version in file: " + filePath.string() + " (this build reads versions " + std::to_string(ProgramHeader::MinimumSupportedVersion) + " to " + std::to_string(ProgramHeader::CurrentVersion) + "; reassemble the source)");

		std::vector<Program::ByteType> text(header.textSize);
		file.read(reinterpret_cast<char*>(text.data()), header.textSize);
		if (!file)
			return std::unexpected("Failed to read text segment from file: " + filePath.string());

		std::vector<Program::ByteType> rodata(header.rodataSize);
		file.read(reinterpret_cast<char*>(rodata.data()), header.rodataSize);
		if (!file)
			return std::unexpected("Failed to read rodata segment from file: " + filePath.string());

		std::vector<Program::ByteType> data(header.dataSize);
		file.read(reinterpret_cast<char*>(data.data()), header.dataSize);
		if (!file)
			return std::unexpected("Failed to read data segment from file: " + filePath.string());

		return Program(header, std::move(text), std::move(rodata), std::move(data), readDebugSection(file, header));
	}

	std::expected<Program, std::string> Program::loadFromBytes(std::span<const ByteType> bytes)
	{
		if (bytes.size() < sizeof(ProgramHeader))
			return std::unexpected("Byte span is too small to contain a valid program header");

		const ProgramHeader* header = reinterpret_cast<const ProgramHeader*>(bytes.data());
		if (header->magic != ProgramHeader::MagicNumber)
			return std::unexpected("Invalid magic number in byte span");

		if (header->version > ProgramHeader::CurrentVersion || header->version < ProgramHeader::MinimumSupportedVersion)
			return std::unexpected("Unsupported version in byte span");

		const size_t expectedSize = sizeof(ProgramHeader) + header->textSize + header->rodataSize + header->dataSize;
		if (bytes.size() < expectedSize)
			return std::unexpected("Byte span is too small to contain the declared text and data segments");

		std::vector<Program::ByteType> text(header->textSize);
		std::copy(bytes.data() + sizeof(ProgramHeader), bytes.data() + sizeof(ProgramHeader) + header->textSize, text.begin());

		std::vector<Program::ByteType> rodata(header->rodataSize);
		std::copy(bytes.data() + sizeof(ProgramHeader) + header->textSize, bytes.data() + sizeof(ProgramHeader) + header->textSize + header->rodataSize, rodata.begin());

		std::vector<Program::ByteType> data(header->dataSize);
		std::copy(bytes.data() + sizeof(ProgramHeader) + header->textSize + header->rodataSize, bytes.data() + expectedSize, data.begin());

		return Program(*header, std::move(text), std::move(rodata), std::move(data), readDebugSection(bytes, expectedSize, *header));
	}

	std::expected<Program, std::string> Program::loadFromStream(std::istream& stream)
	{
		if (!stream)
			return std::unexpected("Invalid input stream");

		ProgramHeader header;
		stream.read(reinterpret_cast<char*>(&header), sizeof(header));
		if (!stream)
			return std::unexpected("Failed to read program header from stream");

		if (header.magic != ProgramHeader::MagicNumber)
			return std::unexpected("Invalid magic number in stream");

		if (header.version > ProgramHeader::CurrentVersion || header.version < ProgramHeader::MinimumSupportedVersion)
			return std::unexpected("Unsupported version in stream");

		std::vector<Program::ByteType> text(header.textSize);
		stream.read(reinterpret_cast<char*>(text.data()), header.textSize);

		if (!stream)
			return std::unexpected("Failed to read text segment from stream");

		std::vector<Program::ByteType> rodata(header.rodataSize);
		stream.read(reinterpret_cast<char*>(rodata.data()), header.rodataSize);

		if (!stream)
			return std::unexpected("Failed to read rodata segment from stream");

		std::vector<Program::ByteType> data(header.dataSize);
		stream.read(reinterpret_cast<char*>(data.data()), header.dataSize);

		if (!stream)
			return std::unexpected("Failed to read data segment from stream");

		return Program(header, std::move(text), std::move(rodata), std::move(data), readDebugSection(stream, header));
	}

	std::expected<Program, std::string> Program::loadFromMemory(const void* memory, usize size)
	{
		if (size < sizeof(ProgramHeader))
			return std::unexpected("Memory block is too small to contain a valid program header");

		const ProgramHeader* header = reinterpret_cast<const ProgramHeader*>(memory);
		if (header->magic != ProgramHeader::MagicNumber)
			return std::unexpected("Invalid magic number in memory block");

		if (header->version > ProgramHeader::CurrentVersion || header->version < ProgramHeader::MinimumSupportedVersion)
			return std::unexpected("Unsupported version in memory block");

		const size_t expectedSize = sizeof(ProgramHeader) + header->textSize + header->rodataSize + header->dataSize;
		if (size < expectedSize)
			return std::unexpected("Memory block is too small to contain the declared text and data segments");

		const ByteType* const basePtr = reinterpret_cast<const ByteType*>(memory);

		std::vector<Program::ByteType> text(header->textSize);
		std::copy(basePtr + sizeof(ProgramHeader), basePtr + sizeof(ProgramHeader) + header->textSize, text.begin());

		std::vector<Program::ByteType> rodata(header->rodataSize);
		std::copy(basePtr + sizeof(ProgramHeader) + header->textSize, basePtr + sizeof(ProgramHeader) + header->textSize + header->rodataSize, rodata.begin());

		std::vector<Program::ByteType> data(header->dataSize);
		std::copy(basePtr + sizeof(ProgramHeader) + header->textSize + header->rodataSize, basePtr + expectedSize, data.begin());

		return Program(*header, std::move(text), std::move(rodata), std::move(data),
			readDebugSection(std::span<const ByteType>(basePtr, size), expectedSize, *header));
	}

	std::expected<Program, std::string> Program::loadFromString(const std::string& str)
	{
		return loadFromBytes(std::span<const ByteType>(reinterpret_cast<const ByteType*>(str.data()), str.size()));
	}
}
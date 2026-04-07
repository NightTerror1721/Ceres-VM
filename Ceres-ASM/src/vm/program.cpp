#include "program.h"
#include <fstream>

namespace ceres::vm
{
	Program Program::make(
		const ProgramHeader& header,
		std::span<const ByteType> text,
		std::span<const ByteType> rodata,
		std::span<const ByteType> data
	)
	{
		return Program(
			header,
			std::vector<ByteType>(text.begin(), text.end()),
			std::vector<ByteType>(rodata.begin(), rodata.end()),
			std::vector<ByteType>(data.begin(), data.end())
		);
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

		if (header.version > ProgramHeader::CurrentVersion || header.version == 0)
			return std::unexpected("Unsupported version in file: " + filePath.string());

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

		return Program(header, std::move(text), std::move(rodata), std::move(data));
	}

	std::expected<Program, std::string> Program::loadFromBytes(std::span<const ByteType> bytes)
	{
		if (bytes.size() < sizeof(ProgramHeader))
			return std::unexpected("Byte span is too small to contain a valid program header");

		const ProgramHeader* header = reinterpret_cast<const ProgramHeader*>(bytes.data());
		if (header->magic != ProgramHeader::MagicNumber)
			return std::unexpected("Invalid magic number in byte span");

		if (header->version > ProgramHeader::CurrentVersion || header->version == 0)
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

		return Program(*header, std::move(text), std::move(rodata), std::move(data));
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

		if (header.version > ProgramHeader::CurrentVersion || header.version == 0)
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

		return Program(header, std::move(text), std::move(rodata), std::move(data));
	}

	std::expected<Program, std::string> Program::loadFromMemory(const void* memory, usize size)
	{
		if (size < sizeof(ProgramHeader))
			return std::unexpected("Memory block is too small to contain a valid program header");

		const ProgramHeader* header = reinterpret_cast<const ProgramHeader*>(memory);
		if (header->magic != ProgramHeader::MagicNumber)
			return std::unexpected("Invalid magic number in memory block");

		if (header->version > ProgramHeader::CurrentVersion || header->version == 0)
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

		return Program(*header, std::move(text), std::move(rodata), std::move(data));
	}

	std::expected<Program, std::string> Program::loadFromString(const std::string& str)
	{
		return loadFromBytes(std::span<const ByteType>(reinterpret_cast<const ByteType*>(str.data()), str.size()));
	}
}
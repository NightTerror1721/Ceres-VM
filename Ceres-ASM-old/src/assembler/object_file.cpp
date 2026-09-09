#include "object_file.h"

#include <format>
#include <fstream>

namespace ceres::casm
{
	namespace
	{
		// Little-endian byte by byte, like the debug section and the instruction encoder: an object
		// assembled on one machine has to mean the same thing on another.
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

		void writeBytes(std::vector<u8>& out, std::span<const u8> bytes)
		{
			writeU32(out, static_cast<u32>(bytes.size()));
			out.insert(out.end(), bytes.begin(), bytes.end());
		}

		void writeString(std::vector<u8>& out, std::string_view text)
		{
			writeU32(out, static_cast<u32>(text.size()));
			out.insert(out.end(), text.begin(), text.end());
		}

		// Every read is checked, and the first failure poisons the rest: a truncated file should
		// produce one error naming the file, not a walk off the end of the buffer.
		struct Reader
		{
			std::span<const u8> bytes;
			usize offset = 0;
			bool failed = false;

			bool has(usize count) const noexcept { return !failed && offset + count <= bytes.size(); }

			u8 readU8() noexcept
			{
				if (!has(1)) { failed = true; return 0; }
				return bytes[offset++];
			}

			u16 readU16() noexcept
			{
				if (!has(2)) { failed = true; return 0; }
				const u16 value = static_cast<u16>(bytes[offset]) | (static_cast<u16>(bytes[offset + 1]) << 8);
				offset += 2;
				return value;
			}

			u32 readU32() noexcept
			{
				if (!has(4)) { failed = true; return 0; }
				const u32 value = static_cast<u32>(bytes[offset]) |
					(static_cast<u32>(bytes[offset + 1]) << 8) |
					(static_cast<u32>(bytes[offset + 2]) << 16) |
					(static_cast<u32>(bytes[offset + 3]) << 24);
				offset += 4;
				return value;
			}

			std::vector<u8> readBytes() noexcept
			{
				const u32 size = readU32();
				if (!has(size)) { failed = true; return {}; }
				std::vector<u8> out(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
					bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
				offset += size;
				return out;
			}

			std::string readString() noexcept
			{
				const u32 size = readU32();
				if (!has(size)) { failed = true; return {}; }
				std::string out(reinterpret_cast<const char*>(bytes.data() + offset), size);
				offset += size;
				return out;
			}
		};

		u32 peekMagic(std::span<const u8> bytes) noexcept
		{
			if (bytes.size() < 4)
				return 0;
			return static_cast<u32>(bytes[0]) |
				(static_cast<u32>(bytes[1]) << 8) |
				(static_cast<u32>(bytes[2]) << 16) |
				(static_cast<u32>(bytes[3]) << 24);
		}

		bool isValidSection(u8 value) noexcept { return value <= static_cast<u8>(SectionType::BSS); }
		bool isValidField(u8 value) noexcept { return value <= static_cast<u8>(RelocationField::SImm24); }

		std::expected<std::vector<u8>, std::string> readWholeFile(const std::filesystem::path& path)
		{
			std::ifstream file(path, std::ios::binary);
			if (!file)
				return std::unexpected(std::format("Could not open '{}'", path.string()));

			std::vector<u8> bytes{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
			return bytes;
		}

		std::expected<void, std::string> writeWholeFile(const std::filesystem::path& path, std::span<const u8> bytes)
		{
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			if (!file)
				return std::unexpected(std::format("Could not open '{}' for writing", path.string()));

			file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if (!file)
				return std::unexpected(std::format("Could not write '{}'", path.string()));

			return {};
		}
	}

	std::vector<u8> ObjectFile::serialize() const
	{
		std::vector<u8> out;

		writeU32(out, MagicNumber);
		writeU16(out, CurrentVersion);
		writeU16(out, 0); // Reserved, so the header stays a whole number of words

		writeString(out, sourceFile);
		writeBytes(out, text);
		writeBytes(out, rodata);
		writeBytes(out, data);
		writeU32(out, bssSize);

		writeU32(out, static_cast<u32>(symbols.size()));
		for (const ObjectSymbol& symbol : symbols)
		{
			writeString(out, symbol.name);
			writeU8(out, static_cast<u8>(symbol.section));
			writeU32(out, symbol.offset);
		}

		writeU32(out, static_cast<u32>(relocations.size()));
		for (const Relocation& relocation : relocations)
		{
			writeU32(out, relocation.offset);
			writeU8(out, static_cast<u8>(relocation.field));
			writeU8(out, relocation.shift);
			writeU8(out, relocation.pcRelative ? 1 : 0);
			writeU8(out, relocation.external ? 1 : 0);
			writeU8(out, static_cast<u8>(relocation.section));
			writeU32(out, static_cast<u32>(relocation.addend));
			writeString(out, relocation.symbol);
		}

		writeBytes(out, debugSection);
		return out;
	}

	std::expected<ObjectFile, std::string> ObjectFile::deserialize(std::span<const u8> bytes)
	{
		Reader reader{ bytes };

		if (reader.readU32() != MagicNumber)
			return std::unexpected("Not a Ceres object file (bad magic number)");

		const u16 version = reader.readU16();
		if (version != CurrentVersion)
			return std::unexpected(std::format("Object file version {} is not supported (this build reads version {})",
				version, CurrentVersion));

		reader.readU16(); // Reserved

		ObjectFile object;
		object.sourceFile = reader.readString();
		object.text = reader.readBytes();
		object.rodata = reader.readBytes();
		object.data = reader.readBytes();
		object.bssSize = reader.readU32();

		const u32 symbolCount = reader.readU32();
		if (reader.failed)
			return std::unexpected("Object file is truncated");

		object.symbols.reserve(symbolCount);
		for (u32 i = 0; i < symbolCount && !reader.failed; ++i)
		{
			ObjectSymbol symbol;
			symbol.name = reader.readString();
			const u8 section = reader.readU8();
			if (!isValidSection(section))
				return std::unexpected(std::format("Object file names an unknown section for symbol '{}'", symbol.name));
			symbol.section = static_cast<SectionType>(section);
			symbol.offset = reader.readU32();
			object.symbols.push_back(std::move(symbol));
		}

		const u32 relocationCount = reader.readU32();
		if (reader.failed)
			return std::unexpected("Object file is truncated");

		object.relocations.reserve(relocationCount);
		for (u32 i = 0; i < relocationCount && !reader.failed; ++i)
		{
			Relocation relocation;
			relocation.offset = reader.readU32();
			const u8 field = reader.readU8();
			if (!isValidField(field))
				return std::unexpected("Object file names an unknown relocation field");
			relocation.field = static_cast<RelocationField>(field);
			relocation.shift = reader.readU8();
			relocation.pcRelative = reader.readU8() != 0;
			relocation.external = reader.readU8() != 0;
			const u8 section = reader.readU8();
			if (!isValidSection(section))
				return std::unexpected("Object file names an unknown section in a relocation");
			relocation.section = static_cast<SectionType>(section);
			relocation.addend = static_cast<i32>(reader.readU32());
			relocation.symbol = reader.readString();
			object.relocations.push_back(std::move(relocation));
		}

		object.debugSection = reader.readBytes();

		if (reader.failed)
			return std::unexpected("Object file is truncated");

		return object;
	}

	bool ObjectFile::looksLikeObject(std::span<const u8> bytes) noexcept
	{
		return peekMagic(bytes) == MagicNumber;
	}

	std::expected<ObjectFile, std::string> ObjectFile::read(const std::filesystem::path& path)
	{
		auto bytes = readWholeFile(path);
		if (!bytes)
			return std::unexpected(bytes.error());

		auto object = deserialize(bytes.value());
		if (!object)
			return std::unexpected(std::format("{}: {}", path.string(), object.error()));

		return object;
	}

	std::expected<void, std::string> ObjectFile::write(const std::filesystem::path& path) const
	{
		return writeWholeFile(path, serialize());
	}

	std::vector<u8> ObjectArchive::serialize() const
	{
		std::vector<u8> out;

		writeU32(out, MagicNumber);
		writeU16(out, CurrentVersion);
		writeU16(out, 0); // Reserved

		writeU32(out, static_cast<u32>(members.size()));
		for (const Member& member : members)
		{
			writeString(out, member.name);
			writeBytes(out, member.object.serialize());
		}

		return out;
	}

	std::expected<ObjectArchive, std::string> ObjectArchive::deserialize(std::span<const u8> bytes)
	{
		Reader reader{ bytes };

		if (reader.readU32() != MagicNumber)
			return std::unexpected("Not a Ceres archive (bad magic number)");

		const u16 version = reader.readU16();
		if (version != CurrentVersion)
			return std::unexpected(std::format("Archive version {} is not supported (this build reads version {})",
				version, CurrentVersion));

		reader.readU16(); // Reserved

		ObjectArchive archive;
		const u32 memberCount = reader.readU32();
		if (reader.failed)
			return std::unexpected("Archive is truncated");

		archive.members.reserve(memberCount);
		for (u32 i = 0; i < memberCount && !reader.failed; ++i)
		{
			Member member;
			member.name = reader.readString();
			const std::vector<u8> memberBytes = reader.readBytes();
			if (reader.failed)
				break;

			auto object = ObjectFile::deserialize(memberBytes);
			if (!object)
				return std::unexpected(std::format("archive member '{}': {}", member.name, object.error()));

			member.object = std::move(object.value());
			archive.members.push_back(std::move(member));
		}

		if (reader.failed)
			return std::unexpected("Archive is truncated");

		return archive;
	}

	bool ObjectArchive::looksLikeArchive(std::span<const u8> bytes) noexcept
	{
		return peekMagic(bytes) == MagicNumber;
	}

	std::expected<ObjectArchive, std::string> ObjectArchive::read(const std::filesystem::path& path)
	{
		auto bytes = readWholeFile(path);
		if (!bytes)
			return std::unexpected(bytes.error());

		auto archive = deserialize(bytes.value());
		if (!archive)
			return std::unexpected(std::format("{}: {}", path.string(), archive.error()));

		return archive;
	}

	std::expected<void, std::string> ObjectArchive::write(const std::filesystem::path& path) const
	{
		return writeWholeFile(path, serialize());
	}

	std::expected<std::vector<ObjectArchive::Member>, std::string> readObjectsFrom(const std::filesystem::path& path)
	{
		auto bytes = readWholeFile(path);
		if (!bytes)
			return std::unexpected(bytes.error());

		if (ObjectFile::looksLikeObject(bytes.value()))
		{
			auto object = ObjectFile::deserialize(bytes.value());
			if (!object)
				return std::unexpected(std::format("{}: {}", path.string(), object.error()));

			std::vector<ObjectArchive::Member> members;
			members.push_back(ObjectArchive::Member{ path.filename().string(), std::move(object.value()) });
			return members;
		}

		if (ObjectArchive::looksLikeArchive(bytes.value()))
		{
			auto archive = ObjectArchive::deserialize(bytes.value());
			if (!archive)
				return std::unexpected(std::format("{}: {}", path.string(), archive.error()));

			std::vector<ObjectArchive::Member> members = std::move(archive.value().members);
			for (ObjectArchive::Member& member : members)
				member.fromArchive = true;

			return members;
		}

		return std::unexpected(std::format("'{}' is neither an object file nor an archive", path.string()));
	}
}

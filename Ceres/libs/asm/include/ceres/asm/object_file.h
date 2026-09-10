#pragma once

// A translation unit assembled on its own, and an archive of several of them.
//
// A `.cres` says where everything is. A `.cobj` says where everything is *relative to its own
// sections*, plus two lists the program file has no need of: what this unit defines that others may
// use, and what it left blank for the link to fill in. That is the whole difference.

#include "relocation.h"
#include <ceres/core/base/types.h>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace ceres::casm
{
	// One name this object publishes. No type information travels with it: a caller learns what a
	// symbol *is* from the source it imports, the way a header works, and the link only needs to
	// know where it ended up.
	struct ObjectSymbol
	{
		std::string name;
		SectionType section = SectionType::Text;
		u32 offset = 0; // Within its own section
	};

	class ObjectFile
	{
	public:
		// 'CASO' in ASCII - Ceres ASsembly Object - by the same convention as ProgramHeader's
		// 'CRES' and the debug section's 'CDBG'.
		static inline constexpr u32 MagicNumber = 0x4341534F;
		// 1 -> 2: added the interrupt-binding table (ObjectInterruptBinding), so a v1 reader would
		// silently never see a program's `interrupt` declarations rather than losing them loudly.
		static inline constexpr u16 CurrentVersion = 2;

	public:
		// Where it came from. Carried for diagnostics only: "undefined symbol" is nearly useless
		// without saying which object was looking for it.
		std::string sourceFile;

		std::vector<u8> text;
		std::vector<u8> rodata;
		std::vector<u8> data;
		u32 bssSize = 0;

		std::vector<ObjectSymbol> symbols;
		std::vector<Relocation> relocations;
		// This unit's `interrupt` declarations, not yet resolved to a final address - see
		// ObjectInterruptBinding. Empty for an object that declares none, which is what keeps such
		// an object's serialized form unchanged from before this field existed.
		std::vector<ObjectInterruptBinding> interruptBindings;

		// The unit's own debug tables, with addresses still relative to its sections. Empty unless
		// the object was assembled with --debug.
		std::vector<u8> debugSection;

	public:
		std::vector<u8> serialize() const;
		static std::expected<ObjectFile, std::string> deserialize(std::span<const u8> bytes);

		static std::expected<ObjectFile, std::string> read(const std::filesystem::path& path);
		std::expected<void, std::string> write(const std::filesystem::path& path) const;

		// True when the file starts with an object's magic number, which is how one command can
		// take both objects and archives without being told which is which.
		static bool looksLikeObject(std::span<const u8> bytes) noexcept;
	};

	// Several objects in one file, with an index of what each of them defines. A link pulls in the
	// members that answer a name nothing else has answered, and leaves the rest out - which is the
	// point of an archive rather than a list of objects: a program that calls one routine from a
	// library does not carry the other forty.
	class ObjectArchive
	{
	public:
		// 'CARC' - Ceres ARChive.
		static inline constexpr u32 MagicNumber = 0x43415243;
		static inline constexpr u16 CurrentVersion = 1;

		struct Member
		{
			std::string name; // How the member is named in diagnostics: its object's file name
			ObjectFile object;
			// Set for a member that came out of an archive, which is what makes it optional: an
			// object named on the command line is part of the program whether anything calls it.
			bool fromArchive = false;
		};

	public:
		std::vector<Member> members;

	public:
		std::vector<u8> serialize() const;
		static std::expected<ObjectArchive, std::string> deserialize(std::span<const u8> bytes);

		static std::expected<ObjectArchive, std::string> read(const std::filesystem::path& path);
		std::expected<void, std::string> write(const std::filesystem::path& path) const;

		static bool looksLikeArchive(std::span<const u8> bytes) noexcept;
	};

	// Reads whatever is at `path` - an object or an archive - and returns its members in order.
	// Anything else is an error naming what was found instead.
	std::expected<std::vector<ObjectArchive::Member>, std::string> readObjectsFrom(const std::filesystem::path& path);
}

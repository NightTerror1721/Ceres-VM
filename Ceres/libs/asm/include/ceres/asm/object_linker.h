#pragma once

// Turning objects into a program: place each one's sections, decide what every name means, and
// fill in the fields the assembler left blank.
//
// This is the second half of the job the in-memory linker does in one go. The difference is what it
// has to work from: not an AST it can ask questions of, but bytes and a list of notes about them.

#include "object_file.h"
#include <ceres/core/format/program.h>
#include <ceres/core/format/debug_info.h>
#include <span>
#include <string>
#include <vector>

namespace ceres::casm
{
	using namespace fmt;

	struct ObjectLinkOptions
	{
		// A library being packaged has no entry point and is not wrong for it; a program is.
		bool requireEntryPoint = true;
		// Merges each object's debug tables into one set covering the whole program. Costs nothing
		// when the objects carry none.
		bool emitDebugInfo = false;
		// Appends to .rodata a table of every global name in .text and its address, sorted by address,
		// between __symtab_start and __symtab_end, so the program itself can say which function an address
		// is in - a backtrace, a fault report. Without it the two names are the same address: no entries.
		//   u32 count; { u32 address; u32 nameOffset; } [count]; the names, NUL-terminated
		// nameOffset counts from __symtab_start.
		bool emitSymbolTable = false;
		// Drops the functions nothing reaches. An object's .text is cut at its global names into pieces, and a
		// piece is kept when the entry point, an interrupt binding, a word in .rodata or .data, or a kept piece
		// refers to it - or when the piece before it is kept and does not end in a jump or a return, since it
		// could run on into it (the piece before an object's first is the previous object's last). Only an
		// object that records every reference within its .text is cut (ObjectFile::FlagCompleteTextRelocations);
		// any other is kept whole. With no entry point (a library) every global name is kept. Off with
		// emitDebugInfo, whose tables would still point where the code was, and when a name is defined twice.
		bool gcSections = false;
	};

	class ObjectLinker
	{
	private:
		std::vector<std::string> _errors;
		DebugInfo _debugInfo;

	public:
		ObjectLinker() = default;
		ObjectLinker(const ObjectLinker&) = delete;
		ObjectLinker(ObjectLinker&&) = default;
		~ObjectLinker() = default;

		ObjectLinker& operator=(const ObjectLinker&) = delete;
		ObjectLinker& operator=(ObjectLinker&&) = default;

	public:
		// Objects that came from a `.cobj` are always part of the program. Ones that came from an
		// archive are pulled in only when they answer a name nothing else does - that being the
		// whole difference between an archive and a list of objects.
		std::optional<Program> link(std::vector<ObjectArchive::Member> inputs, const ObjectLinkOptions& options = {});

		std::span<const std::string> errors() const noexcept { return _errors; }
		bool hasErrors() const noexcept { return !_errors.empty(); }

		// Only meaningful after a link that was asked for it; empty otherwise.
		DebugInfo takeDebugInfo() noexcept { return std::move(_debugInfo); }

	private:
		void reportError(std::string message) { _errors.push_back(std::move(message)); }

	public:
		// What --gc-sections left out of the last link, in bytes of .text.
		u32 bytesCollected() const noexcept { return _bytesCollected; }

	private:
		u32 _bytesCollected = 0;
		void collectUnusedCode(std::vector<ObjectArchive::Member*>& members);
	};
}

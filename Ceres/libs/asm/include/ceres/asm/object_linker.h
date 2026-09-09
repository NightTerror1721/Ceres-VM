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
	};
}

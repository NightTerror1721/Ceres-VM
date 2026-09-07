// Imports: a module's constants and macros become visible to the file importing it, paths
// resolve relative to the importer, and a cycle is reported instead of recursing.

#include "framework.h"
#include "assemble_helper.h"
#include <filesystem>
#include <fstream>
#include <string>

using namespace ceres;
using namespace ceres::testing;

namespace
{
	// Writes a set of files into a scratch directory and assembles the first one, so imports
	// have real paths to resolve.
	struct Workspace
	{
		std::filesystem::path root;

		explicit Workspace(std::string_view name)
			: root(std::filesystem::temp_directory_path() / std::format("ceres_modules_{}", name))
		{
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
			std::filesystem::create_directories(root, ignored);
		}

		~Workspace()
		{
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}

		Workspace(const Workspace&) = delete;
		Workspace& operator=(const Workspace&) = delete;

		void write(std::string_view relativePath, std::string_view contents) const
		{
			const std::filesystem::path full = root / relativePath;
			std::error_code ignored;
			std::filesystem::create_directories(full.parent_path(), ignored);

			std::ofstream file(full, std::ios::binary | std::ios::trunc);
			file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		}

		AssembleResult assemble(std::string_view entry) const
		{
			AssembleResult result;

			casm::Assembler assembler{};
			result.program = assembler.assemble({ root / entry });
			for (const auto& error : assembler.errors())
				result.errors.push_back(std::format("[line {}] {}", error.line, error.message));

			return result;
		}
	};
}

TEST(modules, an_imported_constant_is_visible)
{
	Workspace ws{ "constant" };
	ws.write("consts.casm",
		"const ANSWER = 42\r\n");
	ws.write("main.casm",
		"import \"consts.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, ANSWER\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(vm::Instruction(r.words()[0]).imm16(), u16{ 42 });
}

TEST(modules, an_imported_macro_can_be_called)
{
	Workspace ws{ "macro" };
	ws.write("lib.casm",
		"macro set_to_seven $reg\r\n"
		"    li $reg, 7\r\n"
		"endmacro\r\n");
	ws.write("main.casm",
		"import \"lib.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    set_to_seven r5\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(vm::Instruction(r.words()[0]).rd(), u8{ 5 });
	CHECK_EQ(vm::Instruction(r.words()[0]).imm16(), u16{ 7 });
}

TEST(modules, a_path_resolves_relative_to_the_importing_file)
{
	// The importer sits in a subdirectory and refers to a sibling. Resolving against the working
	// directory instead would not find it.
	Workspace ws{ "relative" };
	ws.write("lib/values.casm",
		"const LOCAL_VALUE = 99\r\n");
	ws.write("lib/wrapper.casm",
		"import \"values.casm\"\r\n");
	ws.write("main.casm",
		"import \"lib/wrapper.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, LOCAL_VALUE\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(vm::Instruction(r.words()[0]).imm16(), u16{ 99 });
}

TEST(modules, importing_the_same_module_twice_is_not_an_error)
{
	Workspace ws{ "twice" };
	ws.write("consts.casm", "const V = 3\r\n");
	ws.write("main.casm",
		"import \"consts.casm\"\r\n"
		"import \"consts.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, V\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(r.ok());
	if (!r.ok()) Registry::instance().recordFailure(r.joinedErrors());
}

TEST(modules, an_import_cycle_is_reported_instead_of_recursing)
{
	Workspace ws{ "cycle" };
	ws.write("a.casm",
		"import \"b.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");
	ws.write("b.casm",
		"import \"a.casm\"\r\n");

	AssembleResult r = ws.assemble("a.casm");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("cycle") != std::string::npos);
}

TEST(modules, a_missing_module_is_reported_with_the_path_it_looked_for)
{
	Workspace ws{ "missing" };
	ws.write("main.casm",
		"import \"nowhere.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("nowhere.casm") != std::string::npos);
}

// A diamond - main imports two modules that both import a third - used to fail with "Redefinition
// of global symbol", because each import copied the whole table of the module it imported, so the
// shared module's declarations arrived twice by two different routes. Nothing is copied any more.
TEST(modules, a_module_reached_through_two_paths_is_not_a_redefinition)
{
	Workspace ws{ "diamond" };
	ws.write("shared.casm",
		"const SHARED = 7\r\n"
		"macro shared_nop\r\n"
		"    nop\r\n"
		"endmacro\r\n");
	ws.write("left.casm",
		"import \"shared.casm\"\r\n"
		"const FROM_LEFT = 1\r\n");
	ws.write("right.casm",
		"import \"shared.casm\"\r\n"
		"const FROM_RIGHT = 2\r\n");
	ws.write("main.casm",
		"import \"left.casm\"\r\n"
		"import \"right.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, SHARED\r\n"
		"    li r2, FROM_LEFT\r\n"
		"    li r3, FROM_RIGHT\r\n"
		"    shared_nop\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	// li r1, 7 - the shared constant reached through both branches still has its one value.
	CHECK_EQ(vm::Instruction(r.words()[0]).imm16(), u16{ 7 });
}

// The other half of not merging: a name that genuinely has two different definitions is no longer
// caught at import time, so it has to be caught where it is used - and say where both came from.
TEST(modules, two_modules_exporting_the_same_name_are_reported_at_the_use)
{
	Workspace ws{ "ambiguous" };
	ws.write("left.casm", "const CLASH = 1\r\n");
	ws.write("right.casm", "const CLASH = 2\r\n");
	ws.write("main.casm",
		"import \"left.casm\"\r\n"
		"import \"right.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, CLASH\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("CLASH") != std::string::npos);
	CHECK(r.joinedErrors().find("left.casm") != std::string::npos);
	CHECK(r.joinedErrors().find("right.casm") != std::string::npos);
}

// Two modules may each declare the same name as long as nothing forces a choice between them.
TEST(modules, an_unused_clash_between_two_modules_is_not_an_error)
{
	Workspace ws{ "unused_clash" };
	ws.write("left.casm", "const CLASH = 1\r\nconst LEFT_ONLY = 10\r\n");
	ws.write("right.casm", "const CLASH = 2\r\n");
	ws.write("main.casm",
		"import \"left.casm\"\r\n"
		"import \"right.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, LEFT_ONLY\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(r.ok());
	if (!r.ok()) Registry::instance().recordFailure(r.joinedErrors());
}

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

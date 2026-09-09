// Imports: a module's constants and macros become visible to the file importing it, paths
// resolve relative to the importer, and a cycle is reported instead of recursing.

#include "framework.h"
#include "assemble_helper.h"
#include <filesystem>
#include <fstream>
#include <string>

using namespace ceres;
using namespace ceres::isa;
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
			for (const auto& diagnostic : assembler.errors())
			{
				auto& into = diagnostic.isWarning() ? result.warnings : result.errors;
				into.push_back(std::format("[line {}] {}", diagnostic.line, diagnostic.message));
			}

			return result;
		}
	};
}

TEST(modules, an_imported_constant_is_visible)
{
	Workspace ws{ "constant" };
	ws.write("consts.casm",
		"global const ANSWER = 42\r\n");
	ws.write("main.casm",
		"import \"consts.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, ANSWER\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 42 });
}

TEST(modules, an_imported_macro_can_be_called)
{
	Workspace ws{ "macro" };
	ws.write("lib.casm",
		"global macro set_to_seven $reg\r\n"
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

	CHECK_EQ(Instruction(r.words()[0]).rd(), u8{ 5 });
	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 7 });
}

TEST(modules, a_path_resolves_relative_to_the_importing_file)
{
	// The importer sits in a subdirectory and refers to a sibling. Resolving against the working
	// directory instead would not find it.
	Workspace ws{ "relative" };
	ws.write("lib/values.casm",
		"global const LOCAL_VALUE = 99\r\n");
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

	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 99 });
}

TEST(modules, importing_the_same_module_twice_is_not_an_error)
{
	Workspace ws{ "twice" };
	ws.write("consts.casm", "global const V = 3\r\n");
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
		"global const SHARED = 7\r\n"
		"global macro shared_nop\r\n"
		"    nop\r\n"
		"endmacro\r\n");
	ws.write("left.casm",
		"import \"shared.casm\"\r\n"
		"global const FROM_LEFT = 1\r\n");
	ws.write("right.casm",
		"import \"shared.casm\"\r\n"
		"global const FROM_RIGHT = 2\r\n");
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
	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 7 });
}

// The other half of not merging: a name that genuinely has two different definitions is no longer
// caught at import time, so it has to be caught where it is used - and say where both came from.
TEST(modules, two_modules_exporting_the_same_name_are_reported_at_the_use)
{
	Workspace ws{ "ambiguous" };
	ws.write("left.casm", "global const CLASH = 1\r\n");
	ws.write("right.casm", "global const CLASH = 2\r\n");
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
	ws.write("left.casm", "global const CLASH = 1\r\nglobal const LEFT_ONLY = 10\r\n");
	ws.write("right.casm", "global const CLASH = 2\r\n");
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

// The rule the `global` prefix buys: nothing leaves the unit that declares it unless it says so.
TEST(modules, a_constant_without_global_stays_in_its_module)
{
	Workspace ws{ "private_const" };
	ws.write("consts.casm", "const PRIVATE = 5\r\n");
	ws.write("main.casm",
		"import \"consts.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, PRIVATE\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(!r.ok());
	// The name exists, it is just not exported - saying so beats "Unresolved symbol".
	CHECK(r.joinedErrors().find("not global") != std::string::npos);
	CHECK(r.joinedErrors().find("consts.casm") != std::string::npos);
}

TEST(modules, a_macro_without_global_stays_in_its_module)
{
	Workspace ws{ "private_macro" };
	ws.write("helpers.casm",
		"macro do_nothing\r\n"
		"    nop\r\n"
		"endmacro\r\n");
	ws.write("main.casm",
		"import \"helpers.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    do_nothing\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("not global") != std::string::npos);
}

// Variables never crossed an import at all before; a global one now does, addresses and all.
TEST(modules, a_global_variable_is_visible_across_an_import)
{
	Workspace ws{ "global_var" };
	ws.write("state.casm",
		"@data\r\n"
		"    global let counter: u32 = 11\r\n");
	ws.write("main.casm",
		"import \"state.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ldv r1, counter\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(r.ok());
	if (!r.ok()) Registry::instance().recordFailure(r.joinedErrors());
}

TEST(modules, a_variable_without_global_stays_in_its_module)
{
	Workspace ws{ "private_var" };
	ws.write("state.casm",
		"@data\r\n"
		"    let counter: u32 = 11\r\n");
	ws.write("main.casm",
		"import \"state.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ldv r1, counter\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("not global") != std::string::npos);
}

// A global declaration is genuinely exported, so it keeps travelling: main imports middle, which
// imports base, and base's global constant is visible in main without main importing it directly.
TEST(modules, a_global_constant_travels_through_an_intermediate_module)
{
	Workspace ws{ "transitive" };
	ws.write("base.casm", "global const DEEP = 21\r\n");
	ws.write("middle.casm", "import \"base.casm\"\r\n");
	ws.write("main.casm",
		"import \"middle.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, DEEP\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 21 });
}

// Named imports: the escape hatch for two modules that export the same name.
TEST(modules, a_named_import_disambiguates_a_clash)
{
	Workspace ws{ "named" };
	ws.write("math.casm",
		"global const LIMIT = 10\r\n"
		"global macro bump $r\r\n"
		"    inc $r\r\n"
		"endmacro\r\n");
	ws.write("fixed.casm", "global const LIMIT = 256\r\n");
	ws.write("main.casm",
		"import \"math.casm\"  as math\r\n"
		"import \"fixed.casm\" as fx\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, math.LIMIT\r\n"
		"    li r2, fx.LIMIT\r\n"
		"    math.bump r1\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 10 });
	CHECK_EQ(Instruction(r.words()[1]).imm16(), u16{ 256 });
	CHECK_EQ(Instruction(r.words()[2]).opcode() == Opcode::ADDI, true);
}

TEST(modules, a_qualified_name_says_which_half_is_wrong)
{
	Workspace ws{ "qualified_errors" };
	ws.write("lib.casm", "const HIDDEN = 1\r\nglobal const SHOWN = 2\r\n");
	ws.write("unknown.casm",
		"import \"lib.casm\" as lib\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, nope.SHOWN\r\n"
		"    ret\r\n");
	ws.write("private.casm",
		"import \"lib.casm\" as lib\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, lib.HIDDEN\r\n"
		"    ret\r\n");

	AssembleResult unknownModule = ws.assemble("unknown.casm");
	CHECK(!unknownModule.ok());
	CHECK(unknownModule.joinedErrors().find("No import is named 'nope'") != std::string::npos);

	AssembleResult privateName = ws.assemble("private.casm");
	CHECK(!privateName.ok());
	CHECK(privateName.joinedErrors().find("does not export 'HIDDEN'") != std::string::npos);
}

TEST(modules, a_global_register_alias_travels_with_its_module)
{
	// An alias is substituted where it is written, so it has to be known while the importing file
	// is being parsed rather than looked up afterwards like every other symbol. That it works at
	// all is the whole point of this test.
	Workspace ws{ "alias" };
	ws.write("regs.casm",
		"global alias cursor = r5\r\n"
		"alias hidden = r6\r\n");
	ws.write("main.casm",
		"import \"regs.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li cursor, 9\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(Instruction(r.words()[0]).rd(), u8{ 5 });
	CHECK_EQ(Instruction(r.words()[0]).imm16(), u16{ 9 });
}

TEST(modules, an_alias_without_global_stays_in_its_own_file)
{
	Workspace ws{ "aliasprivate" };
	ws.write("regs.casm",
		"global alias cursor = r5\r\n"
		"alias hidden = r6\r\n");
	ws.write("main.casm",
		"import \"regs.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li hidden, 1\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	// Not a register, so it reads as a symbol - and there is no such symbol.
	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("hidden") != std::string::npos);
}

TEST(modules, a_global_alias_reaches_through_the_module_that_imported_it)
{
	// Globals are transitive here: a module that imports another publishes what it saw, so an
	// alias arrives through however many files it has to pass.
	Workspace ws{ "aliasdeep" };
	ws.write("inner.casm", "global alias tally = r7\r\n");
	ws.write("middle.casm", "import \"inner.casm\"\r\n");
	ws.write("main.casm",
		"import \"middle.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li tally, 3\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(Instruction(r.words()[0]).rd(), u8{ 7 });
}

TEST(modules, an_imported_alias_works_where_a_register_has_to_be_known_to_parse_at_all)
{
	// `[cursor + 4]` is a register base or a symbolic address depending on what `cursor` is, and
	// the parser decides that before any symbol table exists. This is the case that made the
	// aliases have to arrive before parsing rather than after it.
	Workspace ws{ "aliasmemory" };
	ws.write("regs.casm", "global alias cursor = r5\r\n");
	ws.write("main.casm",
		"import \"regs.casm\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ldr r1, [cursor + 4]\r\n"
		"    ret\r\n");

	AssembleResult r = ws.assemble("main.casm");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CHECK_EQ(Instruction(r.words()[0]).rs(), u8{ 5 });
	CHECK_EQ(Instruction(r.words()[0]).simm16(), i16{ 4 });
}

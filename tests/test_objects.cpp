// Separate compilation: a unit assembled on its own, and a link that finishes it.
//
// Everything here is about what a unit can know by itself and what it cannot. It knows the shape of
// every access it makes and the offset of everything it declares; it does not know where its own
// sections will be placed, and it does not know the address of anything another unit defines. What
// bridges the two is a relocation.

#include "framework.h"
#include "assembler/assembler.h"
#include "assembler/object_linker.h"
#include "vm/ceresvm.h"
#include "vm/devices.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::testing;

namespace
{
	// A scratch directory of sources, assembled one at a time the way `ceres asm -c` does.
	struct ObjectWorkspace
	{
		std::filesystem::path root;
		std::vector<std::string> errors;

		explicit ObjectWorkspace(std::string_view name)
			: root(std::filesystem::temp_directory_path() / std::format("ceres_objects_{}", name))
		{
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
			std::filesystem::create_directories(root, ignored);
		}

		~ObjectWorkspace()
		{
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}

		ObjectWorkspace(const ObjectWorkspace&) = delete;
		ObjectWorkspace& operator=(const ObjectWorkspace&) = delete;

		void write(std::string_view name, std::string_view contents) const
		{
			std::ofstream file(root / name, std::ios::binary | std::ios::trunc);
			file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		}

		std::optional<casm::ObjectFile> assemble(std::string_view name, bool withDebugInfo = false)
		{
			casm::Assembler assembler{ casm::AssemblerOptions{
				.emitDebugInfo = withDebugInfo,
				.requireEntryPoint = false
			} };

			auto object = assembler.assembleObject(root / name);
			for (const auto& diagnostic : assembler.errors())
			{
				if (!diagnostic.isWarning())
					errors.push_back(std::format("[line {}] {}", diagnostic.line, diagnostic.message));
			}

			if (!errors.empty())
				return std::nullopt;

			return object;
		}

		// Safe when nothing was reported: an empty vector's front() is not a diagnostic.
		std::string firstError() const
		{
			return errors.empty() ? std::string{ "assembly failed without a diagnostic" } : errors.front();
		}
	};

	casm::ObjectArchive::Member memberOf(std::string name, casm::ObjectFile object, bool fromArchive = false)
	{
		casm::ObjectArchive::Member member;
		member.name = std::move(name);
		member.object = std::move(object);
		member.fromArchive = fromArchive;
		return member;
	}

	// Collects what a linked program prints, so a test can check that the addresses the linker
	// filled in were the right ones rather than merely that they were filled in.
	class CapturingTerminal final : public IODevice
	{
	private:
		std::string _output;

	public:
		const std::string& output() const noexcept { return _output; }

		void attachTo(IOPorts& ports)
		{
			ports.attach(default_ports::TERM_STATUS, *this);
			ports.attach(default_ports::TERM_OUT, *this);
			ports.attach(default_ports::TERM_IN, *this);
		}

		u8 readPortUnsignedByte(PortNumber) override { return 0; }
		i8 readPortSignedByte(PortNumber) override { return 0; }
		u16 readPortUnsignedHalfword(PortNumber) override { return 0; }
		i16 readPortSignedHalfword(PortNumber) override { return 0; }
		u32 readPortUnsignedWord(PortNumber) override { return 0; }
		void readPort(PortNumber, Address, u32) override {}

		void writePortByte(PortNumber port, u8 value) override
		{
			if (port == default_ports::TERM_OUT)
				_output.push_back(static_cast<char>(value));
		}
		void writePortHalfword(PortNumber port, u16 value) override { writePortByte(port, static_cast<u8>(value)); }
		void writePortWord(PortNumber port, u32 value) override { writePortByte(port, static_cast<u8>(value)); }
		void writePort(PortNumber port, Address address, u32 size) override
		{
			if (port != default_ports::TERM_OUT || size == 0)
				return;
			for (u8 byte : memory().peekBytes(address, size))
				_output.push_back(static_cast<char>(byte));
		}
	};

	std::string run(const Program& program)
	{
		CeresVM vm{};
		SystemControlDevice sysctl{ [&vm]() { vm.shutdown(); }, [&vm]() { vm.shutdown(); } };
		sysctl.attachTo(vm.io());

		CapturingTerminal terminal{};
		terminal.attachTo(vm.io());

		if (auto loaded = vm.loadProgram(program); !loaded)
			return std::string{ "failed to load: " } + loaded.error();

		(void)vm.run();
		return terminal.output();
	}

	// A library and a program that uses it: a routine to call, a variable to share, and a string
	// that lives in the caller. Between them they exercise every kind of reference an object can
	// make - a call across objects, an address built in two instructions, and a load of a
	// variable another unit declared.
	constexpr std::string_view LibrarySource =
		"const TERM_OUT = 0x01\r\n"
		"\r\n"
		"@data\r\n"
		"    global let calls: u32 = 0\r\n"
		"\r\n"
		"@text\r\n"
		"global say:\r\n"
		"    outm TERM_OUT, r1, r2\r\n"
		"    ldv r4, calls\r\n"
		"    add r4, r4, 1\r\n"
		"    stv calls, r4\r\n"
		"    ret\r\n";

	constexpr std::string_view ProgramSource =
		"import \"lib.casm\"\r\n"
		"\r\n"
		"@rodata\r\n"
		"    let MESSAGE: u8[] = \"two objects\"\r\n"
		"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    la r1, MESSAGE\r\n"
		"    li r2, 11\r\n"
		"    call say\r\n"
		"    ldv r5, calls\r\n"
		"    add r5, r5, '0'\r\n"
		"    outb 0x01, r5\r\n"
		"    li r0, 1\r\n"
		"    outb 0xFF, r0\r\n"
		"    ret\r\n";
}

TEST(objects, an_object_publishes_what_it_defines_and_asks_for_what_it_does_not)
{
	ObjectWorkspace ws{ "publishes" };
	ws.write("lib.casm", LibrarySource);
	ws.write("main.casm", ProgramSource);

	auto library = ws.assemble("lib.casm");
	CHECK(library.has_value());
	if (!library) { Registry::instance().recordFailure(ws.firstError()); return; }

	// Two globals, and nothing else: the private machinery of a unit is nobody else's business.
	CHECK_EQ(library->symbols.size(), usize{ 2 });

	auto program = ws.assemble("main.casm");
	CHECK(program.has_value());
	if (!program) { Registry::instance().recordFailure(ws.firstError()); return; }

	// `say` and `calls` come from the library, so the program has to name them; `MESSAGE` is its
	// own, so its relocations carry a section and an offset instead.
	bool asksForSay = false;
	bool asksForCalls = false;
	bool reachesItsOwnRodata = false;
	for (const casm::Relocation& relocation : program->relocations)
	{
		if (relocation.symbol == "say") asksForSay = true;
		if (relocation.symbol == "calls") asksForCalls = true;
		if (!relocation.isExternal() && relocation.section == casm::SectionType::Rodata)
			reachesItsOwnRodata = true;
	}

	CHECK(asksForSay);
	CHECK(asksForCalls);
	CHECK(reachesItsOwnRodata);

	// Only `main` is published: the string is private to this unit.
	CHECK_EQ(program->symbols.size(), usize{ 1 });
	if (!program->symbols.empty())
		CHECK_EQ(program->symbols.front().name, std::string{ "main" });
}

TEST(objects, two_objects_link_into_a_program_that_runs)
{
	ObjectWorkspace ws{ "link" };
	ws.write("lib.casm", LibrarySource);
	ws.write("main.casm", ProgramSource);

	auto library = ws.assemble("lib.casm");
	auto program = ws.assemble("main.casm");
	CHECK(library.has_value() && program.has_value());
	if (!library || !program) { Registry::instance().recordFailure(ws.firstError()); return; }

	std::vector<casm::ObjectArchive::Member> inputs;
	inputs.push_back(memberOf("main.cobj", std::move(program.value())));
	inputs.push_back(memberOf("lib.cobj", std::move(library.value())));

	casm::ObjectLinker linker;
	auto linked = linker.link(std::move(inputs));

	CHECK(linked.has_value());
	if (!linked) { Registry::instance().recordFailure(linker.errors().front()); return; }

	// The string came out of one object and the routine that prints it out of another, and the
	// counter they share was written by one and read by the other.
	CHECK_EQ(run(linked.value()), std::string{ "two objects1" });
}

TEST(objects, the_order_the_objects_are_given_in_does_not_change_the_program)
{
	ObjectWorkspace ws{ "order" };
	ws.write("lib.casm", LibrarySource);
	ws.write("main.casm", ProgramSource);

	auto library = ws.assemble("lib.casm");
	auto program = ws.assemble("main.casm");
	CHECK(library.has_value() && program.has_value());
	if (!library || !program) { Registry::instance().recordFailure(ws.firstError()); return; }

	std::vector<casm::ObjectArchive::Member> reversed;
	reversed.push_back(memberOf("lib.cobj", library.value()));
	reversed.push_back(memberOf("main.cobj", program.value()));

	casm::ObjectLinker linker;
	auto linked = linker.link(std::move(reversed));

	CHECK(linked.has_value());
	if (!linked) { Registry::instance().recordFailure(linker.errors().front()); return; }

	// The layout differs - `main` is no longer first - and the program does the same thing, which
	// is the whole promise of relocating rather than assuming.
	CHECK_EQ(run(linked.value()), std::string{ "two objects1" });
}

TEST(objects, an_archive_member_nobody_needs_is_left_out)
{
	ObjectWorkspace ws{ "archive" };
	ws.write("lib.casm", LibrarySource);
	ws.write("main.casm", ProgramSource);
	ws.write("extra.casm",
		"@text\r\n"
		"global never_called:\r\n"
		"    li r0, 42\r\n"
		"    ret\r\n");

	auto library = ws.assemble("lib.casm");
	auto program = ws.assemble("main.casm");
	auto extra = ws.assemble("extra.casm");
	CHECK(library.has_value() && program.has_value() && extra.has_value());
	if (!library || !program || !extra) { Registry::instance().recordFailure(ws.firstError()); return; }

	// Copied rather than moved: the same three objects get linked a second time below.
	std::vector<casm::ObjectArchive::Member> inputs;
	inputs.push_back(memberOf("main.cobj", program.value()));
	inputs.push_back(memberOf("lib.cobj", library.value(), true));
	inputs.push_back(memberOf("extra.cobj", extra.value(), true));

	casm::ObjectLinker linker;
	auto linked = linker.link(std::move(inputs));

	CHECK(linked.has_value());
	if (!linked) { Registry::instance().recordFailure(linker.errors().front()); return; }

	// `say` pulled its member in; nothing wanted `never_called`, so its two words are not here.
	const u32 withoutExtra = linked->header().textSize;

	std::vector<casm::ObjectArchive::Member> everything;
	everything.push_back(memberOf("main.cobj", program.value()));
	everything.push_back(memberOf("lib.cobj", library.value()));
	everything.push_back(memberOf("extra.cobj", extra.value()));

	casm::ObjectLinker second;
	auto all = second.link(std::move(everything));
	CHECK(all.has_value());
	if (!all) { Registry::instance().recordFailure(second.errors().front()); return; }

	CHECK(withoutExtra < all->header().textSize);
}

TEST(objects, a_name_nothing_defines_is_a_link_error)
{
	ObjectWorkspace ws{ "undefined" };
	ws.write("lib.casm", LibrarySource);
	ws.write("main.casm", ProgramSource);

	auto program = ws.assemble("main.casm");
	CHECK(program.has_value());
	if (!program) { Registry::instance().recordFailure(ws.firstError()); return; }

	std::vector<casm::ObjectArchive::Member> inputs;
	inputs.push_back(memberOf("main.cobj", std::move(program.value())));

	casm::ObjectLinker linker;
	auto linked = linker.link(std::move(inputs));

	CHECK(!linked.has_value());
	CHECK(linker.hasErrors());

	// Named once each, not once per instruction that wanted it: reaching a variable takes two
	// words and neither of them is a separate mistake.
	usize mentions = 0;
	for (const std::string& error : linker.errors())
	{
		if (error.find("'say'") != std::string::npos)
			++mentions;
	}
	CHECK_EQ(mentions, usize{ 1 });
}

TEST(objects, a_name_two_objects_define_is_a_link_error)
{
	ObjectWorkspace ws{ "duplicate" };
	ws.write("lib.casm", LibrarySource);
	ws.write("main.casm", ProgramSource);

	auto library = ws.assemble("lib.casm");
	auto program = ws.assemble("main.casm");
	CHECK(library.has_value() && program.has_value());
	if (!library || !program) { Registry::instance().recordFailure(ws.firstError()); return; }

	std::vector<casm::ObjectArchive::Member> inputs;
	inputs.push_back(memberOf("main.cobj", std::move(program.value())));
	inputs.push_back(memberOf("lib.cobj", library.value()));
	inputs.push_back(memberOf("lib-again.cobj", library.value()));

	casm::ObjectLinker linker;
	auto linked = linker.link(std::move(inputs));

	CHECK(!linked.has_value());
	CHECK(linker.hasErrors());
}

TEST(objects, a_program_with_no_main_is_a_link_error_and_a_library_without_one_is_not)
{
	ObjectWorkspace ws{ "entry" };
	ws.write("lib.casm", LibrarySource);

	auto library = ws.assemble("lib.casm");
	CHECK(library.has_value());
	if (!library) { Registry::instance().recordFailure(ws.firstError()); return; }

	std::vector<casm::ObjectArchive::Member> inputs;
	inputs.push_back(memberOf("lib.cobj", library.value()));

	casm::ObjectLinker asProgram;
	CHECK(!asProgram.link(std::move(inputs)).has_value());

	std::vector<casm::ObjectArchive::Member> again;
	again.push_back(memberOf("lib.cobj", library.value()));

	casm::ObjectLinker asLibrary;
	CHECK(asLibrary.link(std::move(again), casm::ObjectLinkOptions{ .requireEntryPoint = false }).has_value());
}

TEST(objects, an_object_survives_the_trip_through_its_file_format)
{
	ObjectWorkspace ws{ "roundtrip" };
	ws.write("lib.casm", LibrarySource);
	ws.write("main.casm", ProgramSource);

	auto program = ws.assemble("main.casm");
	CHECK(program.has_value());
	if (!program) { Registry::instance().recordFailure(ws.firstError()); return; }

	const std::vector<u8> bytes = program->serialize();
	auto reread = casm::ObjectFile::deserialize(bytes);

	CHECK(reread.has_value());
	if (!reread) { Registry::instance().recordFailure(reread.error()); return; }

	CHECK_EQ(reread->text.size(), program->text.size());
	CHECK_EQ(reread->rodata.size(), program->rodata.size());
	CHECK_EQ(reread->bssSize, program->bssSize);
	CHECK_EQ(reread->symbols.size(), program->symbols.size());
	CHECK_EQ(reread->relocations.size(), program->relocations.size());

	if (!reread->relocations.empty() && !program->relocations.empty())
	{
		CHECK_EQ(reread->relocations.front().offset, program->relocations.front().offset);
		CHECK_EQ(reread->relocations.front().symbol, program->relocations.front().symbol);
	}

	// A file that is not one says so rather than being read as an empty object.
	const std::vector<u8> nonsense{ 1, 2, 3, 4, 5, 6, 7, 8 };
	CHECK(!casm::ObjectFile::deserialize(nonsense).has_value());
}

TEST(objects, an_archive_holds_its_members_whole)
{
	ObjectWorkspace ws{ "archivefile" };
	ws.write("lib.casm", LibrarySource);

	auto library = ws.assemble("lib.casm");
	CHECK(library.has_value());
	if (!library) { Registry::instance().recordFailure(ws.firstError()); return; }

	casm::ObjectArchive archive;
	archive.members.push_back(memberOf("lib.cobj", library.value()));
	archive.members.push_back(memberOf("lib-again.cobj", library.value()));

	auto reread = casm::ObjectArchive::deserialize(archive.serialize());
	CHECK(reread.has_value());
	if (!reread) { Registry::instance().recordFailure(reread.error()); return; }

	CHECK_EQ(reread->members.size(), usize{ 2 });
	if (reread->members.size() == 2)
	{
		CHECK_EQ(reread->members[0].name, std::string{ "lib.cobj" });
		CHECK_EQ(reread->members[1].object.text.size(), library->text.size());
	}
}

TEST(objects, the_line_table_of_a_linked_program_names_both_of_its_sources)
{
	ObjectWorkspace ws{ "debug" };
	ws.write("lib.casm", LibrarySource);
	ws.write("main.casm", ProgramSource);

	auto library = ws.assemble("lib.casm", true);
	auto program = ws.assemble("main.casm", true);
	CHECK(library.has_value() && program.has_value());
	if (!library || !program) { Registry::instance().recordFailure(ws.firstError()); return; }

	CHECK(!library->debugSection.empty());

	std::vector<casm::ObjectArchive::Member> inputs;
	inputs.push_back(memberOf("main.cobj", std::move(program.value())));
	inputs.push_back(memberOf("lib.cobj", std::move(library.value())));

	casm::ObjectLinker linker;
	auto linked = linker.link(std::move(inputs), casm::ObjectLinkOptions{
		.requireEntryPoint = true,
		.emitDebugInfo = true
	});

	CHECK(linked.has_value());
	if (!linked) { Registry::instance().recordFailure(linker.errors().front()); return; }

	const debug::DebugInfo info = linker.takeDebugInfo();
	CHECK_EQ(info.fileCount(), usize{ 2 });

	// Addresses were moved to where each object's code actually went, so every line entry still
	// lands inside the program's own text.
	const u32 textStart = Memory::UnrestrictedSegmentStart.value();
	bool allInside = !info.lines().empty();
	for (const debug::LineEntry& entry : info.lines())
	{
		if (entry.address < textStart || entry.address >= textStart + linked->header().textSize)
			allInside = false;
	}
	CHECK(allInside);
}

// Separate compilation: a unit assembled on its own, and a link that finishes it.
//
// Everything here is about what a unit can know by itself and what it cannot. It knows the shape of
// every access it makes and the offset of everything it declares; it does not know where its own
// sections will be placed, and it does not know the address of anything another unit defines. What
// bridges the two is a relocation.

#include "framework.h"
#include <ceres/asm/assembler.h>
#include <ceres/asm/object_linker.h>
#include <ceres/vm/ceresvm.h>
#include <ceres/devices/devices.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::devices;
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
		u32 _blockAddress = 0;
		u32 _blockLength = 0;

	public:
		const std::string& output() const noexcept { return _output; }

		void attachTo(MmioBus& bus)
		{
			bus.attach(default_mmio::Terminal, *this);
		}

		u8 readUnsignedByte(Address) override { return 0; }
		i8 readSignedByte(Address) override { return 0; }
		u16 readUnsignedHalfword(Address) override { return 0; }
		i16 readSignedHalfword(Address) override { return 0; }
		u32 readUnsignedWord(Address) override { return 0; }

		void writeByte(Address offset, u8 value) override
		{
			if (offset == TerminalDevice::OutputRegister)
				_output.push_back(static_cast<char>(value));
		}
		void writeHalfword(Address offset, u16 value) override { writeByte(offset, static_cast<u8>(value)); }
		void writeWord(Address offset, u32 value) override
		{
			if (offset == TerminalDevice::BlockAddressRegister) { _blockAddress = value; return; }
			if (offset == TerminalDevice::BlockLengthRegister) { _blockLength = value; return; }
			if (offset == TerminalDevice::BlockCommandRegister)
			{
				if (value == TerminalDevice::BlockCommandWrite)
				{
					for (u8 byte : memory().peekBytes(Address(_blockAddress), _blockLength))
						_output.push_back(static_cast<char>(byte));
				}
				return;
			}
			writeByte(offset, static_cast<u8>(value));
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
		"const TERM_BLOCK_ADDR = 0xFF0000F0\r\n"
		"const TERM_BLOCK_LEN = 0xFF0000F4\r\n"
		"const TERM_BLOCK_CMD = 0xFF0000F8\r\n"
		"\r\n"
		"@data\r\n"
		"    global let calls: u32 = 0\r\n"
		"\r\n"
		"@text\r\n"
		"global say:\r\n"
		"    la r13, TERM_BLOCK_ADDR\r\n"
		"    str [r13 + 0], r1\r\n"
		"    la r13, TERM_BLOCK_LEN\r\n"
		"    str [r13 + 0], r2\r\n"
		"    la r13, TERM_BLOCK_CMD\r\n"
		"    li r12, 2\r\n"
		"    str [r13 + 0], r12\r\n"
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
		"    la r13, 0xFF000004\r\n"
		"    strb [r13 + 0], r5\r\n"
		"    li r0, 1\r\n"
		"    la r13, 0xFFFF0000\r\n"
		"    strb [r13 + 0], r0\r\n"
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

	const DebugInfo info = linker.takeDebugInfo();
	CHECK_EQ(info.fileCount(), usize{ 2 });

	// Addresses were moved to where each object's code actually went, so every line entry still
	// lands inside the program's own text.
	const u32 textStart = Memory::UnrestrictedSegmentStart.value();
	bool allInside = !info.lines().empty();
	for (const LineEntry& entry : info.lines())
	{
		if (entry.address < textStart || entry.address >= textStart + linked->header().textSize)
			allInside = false;
	}
	CHECK(allInside);
}

// --- Interrupt vector binding across objects --------------------------------------------------
//
// An `interrupt` declaration resolves to a placeholder address the moment a unit is assembled on
// its own - it does not yet know where its own code will land, let alone another object's. It
// travels as an ObjectInterruptBinding instead, exactly the way an ordinary address-bearing field
// becomes a Relocation, and only `ceres link` turns it into a real InterruptVectorPatch.

TEST(objects, an_interrupt_binding_reaches_a_handler_defined_in_another_object)
{
	ObjectWorkspace ws{ "interrupt_cross_object" };
	ws.write("lib.casm",
		"@text\r\n"
		"global term_isr:\r\n"
		"    la r13, 0xFF000000\r\n" // Terminal's MMIO base
		"    ldrb r1, [r13 + 8]\r\n" // InputRegister
		"    strb [r13 + 4], r1\r\n" // OutputRegister - echo it straight back
		"    li r0, 1\r\n"
		"    la r13, 0xFFFF0000\r\n" // SystemControl's MMIO base
		"    strb [r13 + 0], r0\r\n" // shut the machine down from inside the handler
		"    iret\r\n");
	ws.write("main.casm",
		"import \"lib.casm\"\r\n"
		"interrupt UserInterrupt1: term_isr\r\n"
		"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    sti\r\n"
		"    halt\r\n");

	auto library = ws.assemble("lib.casm");
	auto program = ws.assemble("main.casm");
	CHECK(library.has_value() && program.has_value());
	if (!library || !program) { Registry::instance().recordFailure(ws.firstError()); return; }

	// The binding is recorded, not resolved: main.casm cannot know term_isr's address until it
	// knows where lib.casm's .text ends up, which is a question only the link can answer.
	CHECK_EQ(program->interruptBindings.size(), usize{ 1 });
	if (!program->interruptBindings.empty())
	{
		CHECK_EQ(program->interruptBindings.front().interruptNumber, u8{ 17 });
		CHECK(program->interruptBindings.front().isExternal());
		CHECK_EQ(program->interruptBindings.front().symbol, std::string{ "term_isr" });
	}

	std::vector<casm::ObjectArchive::Member> inputs;
	inputs.push_back(memberOf("main.cobj", std::move(program.value())));
	inputs.push_back(memberOf("lib.cobj", std::move(library.value())));

	casm::ObjectLinker linker;
	auto linked = linker.link(std::move(inputs));

	CHECK(linked.has_value());
	if (!linked) { Registry::instance().recordFailure(linker.errors().front()); return; }
	CHECK_EQ(linked->interruptVectors().size(), usize{ 1 });

	CeresVM vm{};
	SystemControlDevice sysctl{ [&vm]() { vm.shutdown(); }, [&vm]() { vm.shutdown(); } };
	sysctl.attachTo(vm.io());

	TerminalDevice terminal{};
	terminal.attachTo(vm.io());
	std::string captured;
	terminal.setOutputSink([&captured](u8 byte) { captured.push_back(static_cast<char>(byte)); });

	auto loaded = vm.loadProgram(linked.value());
	CHECK(loaded.has_value());
	if (!loaded) { Registry::instance().recordFailure(loaded.error()); return; }

	// Pushed before run() starts: sti unmasks it on the very next step, so the machine never
	// actually needs to sit halted for this to prove the vector reached the right handler.
	terminal.pushInput('Z');
	(void)vm.run();

	CHECK_EQ(captured, std::string{ "Z" });
}

TEST(objects, the_same_interrupt_bound_in_two_objects_is_a_link_error)
{
	ObjectWorkspace ws{ "interrupt_duplicate" };
	ws.write("a.casm",
		"interrupt UserInterrupt0: handler_a\r\n"
		"@text\r\n"
		"global handler_a:\r\n"
		"    iret\r\n");
	ws.write("b.casm",
		"interrupt UserInterrupt0: handler_b\r\n"
		"@text\r\n"
		"global handler_b:\r\n"
		"    iret\r\n");

	auto a = ws.assemble("a.casm");
	auto b = ws.assemble("b.casm");
	CHECK(a.has_value() && b.has_value());
	if (!a || !b) { Registry::instance().recordFailure(ws.firstError()); return; }

	std::vector<casm::ObjectArchive::Member> inputs;
	inputs.push_back(memberOf("a.cobj", std::move(a.value())));
	inputs.push_back(memberOf("b.cobj", std::move(b.value())));

	casm::ObjectLinker linker;
	auto linked = linker.link(std::move(inputs), casm::ObjectLinkOptions{ .requireEntryPoint = false });

	CHECK(!linked.has_value());
	CHECK(linker.hasErrors());

	bool foundDuplicateError = false;
	for (const std::string& error : linker.errors())
	{
		if (error.find("already bound") != std::string::npos)
			foundDuplicateError = true;
	}
	CHECK(foundDuplicateError);
}

TEST(objects, an_interrupt_bound_to_a_handler_the_link_never_receives_is_a_link_error)
{
	// Mirrors a_name_nothing_defines_is_a_link_error: main.casm assembles cleanly because
	// term_isr is visible through the import, but lib.cobj is never handed to the linker.
	ObjectWorkspace ws{ "interrupt_undefined" };
	ws.write("lib.casm",
		"@text\r\n"
		"global term_isr:\r\n"
		"    iret\r\n");
	ws.write("main.casm",
		"import \"lib.casm\"\r\n"
		"interrupt UserInterrupt1: term_isr\r\n"
		"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ret\r\n");

	auto program = ws.assemble("main.casm");
	CHECK(program.has_value());
	if (!program) { Registry::instance().recordFailure(ws.firstError()); return; }

	std::vector<casm::ObjectArchive::Member> inputs;
	inputs.push_back(memberOf("main.cobj", std::move(program.value())));

	casm::ObjectLinker linker;
	auto linked = linker.link(std::move(inputs));

	CHECK(!linked.has_value());
	CHECK(linker.hasErrors());

	bool foundUndefined = false;
	for (const std::string& error : linker.errors())
	{
		if (error.find("'term_isr'") != std::string::npos)
			foundUndefined = true;
	}
	CHECK(foundUndefined);
}

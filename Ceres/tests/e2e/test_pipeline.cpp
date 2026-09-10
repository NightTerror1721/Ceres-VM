// End-to-end: assemble a program, run it, compare what it printed.
//
// Output is captured through a device rather than by redirecting stdout, so the tests stay
// independent of the process's file descriptors and can run in any order.
//
// Every program below reaches a device through its MMIO address rather than a port number now:
// `la r13, <address>` builds it (AT, r13, is the scratch a real program would use for the same
// job too), and an ordinary `str`/`strb` reaches the register from there.

#include "framework.h"
#include "assemble_helper.h"
#include <ceres/vm/ceresvm.h>
#include <ceres/devices/devices.h>
#include <string>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::devices;
using namespace ceres::testing;

namespace
{
	// Stands in for TerminalDevice, collecting bytes instead of printing them.
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
		void writeHalfword(Address offset, u16 value) override
		{
			writeByte(offset, static_cast<u8>(value & 0xFF));
			writeByte(offset, static_cast<u8>((value >> 8) & 0xFF));
		}
		void writeWord(Address offset, u32 value) override
		{
			if (offset == TerminalDevice::BlockAddressRegister) { _blockAddress = value; return; }
			if (offset == TerminalDevice::BlockLengthRegister) { _blockLength = value; return; }
			if (offset == TerminalDevice::BlockCommandRegister)
			{
				if (value == TerminalDevice::BlockCommandWrite)
				{
					auto bytes = memory().peekBytes(Address(_blockAddress), _blockLength);
					for (u8 byte : bytes)
						_output.push_back(static_cast<char>(byte));
				}
				return;
			}
			for (int i = 0; i < 4; ++i)
				writeByte(offset, static_cast<u8>((value >> (i * 8)) & 0xFF));
		}
	};

	struct RunResult
	{
		bool assembled = false;
		std::string errors;
		std::string output;
	};

	RunResult assembleAndRun(std::string_view source)
	{
		RunResult result;

		AssembleResult assembled = assembleSource(source, "pipeline");
		if (!assembled.ok())
		{
			result.errors = assembled.joinedErrors();
			return result;
		}
		result.assembled = true;

		CeresVM vm{};
		SystemControlDevice sysctl{ [&vm]() { vm.shutdown(); }, [&vm]() { vm.shutdown(); } };
		sysctl.attachTo(vm.io());

		CapturingTerminal terminal{};
		terminal.attachTo(vm.io());

		if (auto loaded = vm.loadProgram(assembled.program.value()); !loaded)
		{
			result.errors = loaded.error();
			result.assembled = false;
			return result;
		}

		(void)vm.run();
		result.output = terminal.output();
		return result;
	}

	// Every program needs to end by writing the shutdown command, or run() never returns.
	constexpr std::string_view shutdown =
		"    la r13, 0xFFFF0000\r\n" // SystemControlDevice's MMIO base (the only register it has)
		"    li r0, 1\r\n"
		"    strb [r13 + 0], r0\r\n";
}

TEST(pipeline, a_program_prints_a_string_and_shuts_down)
{
	RunResult r = assembleAndRun(std::format(
		"@rodata\r\n"
		"    let msg: u8[6] = \"Hola!\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    la r1, msg\r\n"
		"    li r2, 5\r\n"
		"    la r13, 0xFF000000\r\n" // Terminal's MMIO base
		"    str [r13 + 0xF0], r1\r\n" // BlockAddressRegister
		"    str [r13 + 0xF4], r2\r\n" // BlockLengthRegister
		"    li r12, 2\r\n"            // BlockCommandWrite
		"    str [r13 + 0xF8], r12\r\n"
		"{}", shutdown));

	CHECK(r.assembled);
	if (!r.assembled) { Registry::instance().recordFailure(r.errors); return; }
	CHECK_EQ(r.output, std::string{ "Hola!" });
}

TEST(pipeline, a_loop_counts_down_and_terminates)
{
	RunResult r = assembleAndRun(std::format(
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 3\r\n"
		"    li r2, 65\r\n"           // 'A'
		".loop:\r\n"
		"    la r13, 0xFF000004\r\n" // Terminal's OutputRegister
		"    strb [r13 + 0], r2\r\n"
		"    add r2, r2, 1\r\n"
		"    sub r1, r1, 1\r\n"
		"    cmp r1, 0\r\n"
		"    jnz .loop\r\n"
		"{}", shutdown));

	CHECK(r.assembled);
	if (!r.assembled) { Registry::instance().recordFailure(r.errors); return; }
	CHECK_EQ(r.output, std::string{ "ABC" });
}

TEST(pipeline, a_subroutine_returns_to_its_caller)
{
	RunResult r = assembleAndRun(std::format(
		"@text\r\n"
		"global main:\r\n"
		"    call emit\r\n"
		"    li r2, 66\r\n"
		"    la r13, 0xFF000004\r\n"
		"    strb [r13 + 0], r2\r\n"
		"{}"
		"emit:\r\n"
		"    li r2, 65\r\n"
		"    la r13, 0xFF000004\r\n"
		"    strb [r13 + 0], r2\r\n"
		"    ret\r\n", shutdown));

	CHECK(r.assembled);
	if (!r.assembled) { Registry::instance().recordFailure(r.errors); return; }
	CHECK_EQ(r.output, std::string{ "AB" });
}

TEST(pipeline, a_global_variable_survives_a_store_and_a_load)
{
	RunResult r = assembleAndRun(std::format(
		"@data\r\n"
		"    let counter: u32 = 0\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 67\r\n"
		"    stv counter, r1\r\n"
		"    ldv r2, counter\r\n"
		"    la r13, 0xFF000004\r\n"
		"    strb [r13 + 0], r2\r\n"
		"{}", shutdown));

	CHECK(r.assembled);
	if (!r.assembled) { Registry::instance().recordFailure(r.errors); return; }
	CHECK_EQ(r.output, std::string{ "C" });
}

TEST(pipeline, an_indexed_read_uses_its_displacement)
{
	RunResult r = assembleAndRun(std::format(
		"@rodata\r\n"
		"    let letters: u8[5] = \"WXYZ\"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    la r1, letters\r\n"
		"    ldrb r2, [r1 + 2]\r\n"
		"    la r13, 0xFF000004\r\n"
		"    strb [r13 + 0], r2\r\n"
		"{}", shutdown));

	CHECK(r.assembled);
	if (!r.assembled) { Registry::instance().recordFailure(r.errors); return; }
	CHECK_EQ(r.output, std::string{ "Y" });
}

TEST(pipeline, a_dynamically_computed_address_reaches_the_device)
{
	// A device's address does not have to be a compile-time displacement: this builds it with an
	// add instead of baking the offset into the str, the MMIO-era equivalent of what used to be
	// "the port number came from a register instead of an immediate".
	RunResult r = assembleAndRun(std::format(
		"@text\r\n"
		"global main:\r\n"
		"    la r1, 0xFF000000\r\n" // Terminal's MMIO base
		"    li r3, 4\r\n"           // OutputRegister's offset
		"    add r1, r1, r3\r\n"
		"    li r2, 68\r\n"
		"    strb [r1 + 0], r2\r\n"
		"{}", shutdown));

	CHECK(r.assembled);
	if (!r.assembled) { Registry::instance().recordFailure(r.errors); return; }
	CHECK_EQ(r.output, std::string{ "D" });
}

TEST(pipeline, the_shipped_example_still_assembles_and_runs)
{
	// examples/main.casm is the project's own demo; keeping it green stops the language and the
	// example from drifting apart, which is how the u8[17] mismatch survived for so long.
	RunResult r = assembleAndRun(
		"const TERM_OUT = 0xFF000004\r\n"
		"const TERM_BLOCK_ADDR = 0xFF0000F0\r\n"
		"const TERM_BLOCK_LEN = 0xFF0000F4\r\n"
		"const TERM_BLOCK_CMD = 0xFF0000F8\r\n"
		"const EXIT_CODE = 0x01\r\n"
		"const SYS_CTRL = 0xFFFF0000\r\n"
		"\r\n"
		"@rodata\r\n"
		"    let HELLO_MSG: u8[16] = \"Hello, CeresVM!\"\r\n"
		"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    la r1, HELLO_MSG\r\n"
		"    call print\r\n"
		"    li r0, EXIT_CODE\r\n"
		"    la r13, SYS_CTRL\r\n"
		"    strb [r13 + 0], r0\r\n"
		"    ret\r\n"
		"\r\n"
		"print:\r\n"
		"    mov r3, r1\r\n"
		"    call strlen\r\n"
		"    mov r2, r0\r\n"
		"    la r13, TERM_BLOCK_ADDR\r\n"
		"    str [r13 + 0], r3\r\n"
		"    la r13, TERM_BLOCK_LEN\r\n"
		"    str [r13 + 0], r2\r\n"
		"    la r13, TERM_BLOCK_CMD\r\n"
		"    li r12, 2\r\n"
		"    str [r13 + 0], r12\r\n"
		"    ret\r\n"
		"\r\n"
		"strlen:\r\n"
		"    xor r0, r0, r0\r\n"
		".strlen_loop:\r\n"
		"    ldrb r2, [r1]\r\n"
		"    cmp r2, 0\r\n"
		"    jz .strlen_end\r\n"
		"    add r0, r0, 1\r\n"
		"    add r1, r1, 1\r\n"
		"    jp .strlen_loop\r\n"
		".strlen_end:\r\n"
		"    ret\r\n");

	CHECK(r.assembled);
	if (!r.assembled) { Registry::instance().recordFailure(r.errors); return; }
	CHECK_EQ(r.output, std::string{ "Hello, CeresVM!" });
}

TEST(pipeline, a_program_without_a_global_main_is_rejected)
{
	AssembleResult r = assembleSource(
		"@text\r\n"
		"notmain:\r\n"
		"    ret\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("main") != std::string::npos);
}

TEST(pipeline, an_unknown_mnemonic_is_reported_not_silently_dropped)
{
	// Anything that is not a known mnemonic parses as a macro call. Once macro expansion was
	// implemented, a call that resolves to no macro became a diagnostic instead of silence.
	AssembleResult r = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    frobnicate r1, r2\r\n");

	CHECK(!r.ok());
}

// An entry point is a property of a program, not of a translation unit. `ceres asm` with no -o only
// checks the file - which is what the language server runs on every open document - so a library
// module without a `main` must not be reported as broken.
TEST(pipeline, checking_a_file_does_not_demand_an_entry_point)
{
	AssembleResult r = checkSource(
		"const BLOCK = 16\r\n"
		"@rodata\r\n"
		"    let banner: u8[4] = \"lib\"\r\n"
		"@text\r\n"
		"helper:\r\n"
		"    ret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }
}

TEST(pipeline, checking_a_file_still_reports_real_errors)
{
	// Relaxing the entry point must not relax anything else: this diagnostic comes from the
	// emitter, which a check run still has to go through.
	AssembleResult r = checkSource(
		"@text\r\n"
		"helper:\r\n"
		"    li r1, 70000\r\n");

	CHECK(!r.ok());
	CHECK(r.joinedErrors().find("70000") != std::string::npos);
}

// The calling convention end to end: a leaf with no frame, a frame with locals and a call inside
// it, recursion across a callee-saved register, and arguments five and six read from the caller's
// outgoing area. Mirrors examples/calling_convention.casm, which prints 120 then 21.
TEST(pipeline, the_calling_convention_holds_together)
{
	RunResult r = assembleAndRun(
		"@text\r\n"
		"global main:\r\n"
		"    enter\r\n"
		"    sub sp, sp, 8\r\n"          // outgoing area for sum6's fifth and sixth arguments
		"    li  r0, 5\r\n"
		"    call factorial\r\n"
		"    call print_u32\r\n"
		"    li  r0, 1\r\n"
		"    li  r1, 2\r\n"
		"    li  r2, 3\r\n"
		"    li  r3, 4\r\n"
		"    li  r4, 5\r\n"
		"    str [sp + 0], r4\r\n"
		"    li  r4, 6\r\n"
		"    str [sp + 4], r4\r\n"
		"    call sum6\r\n"
		"    call print_u32\r\n"
		"    li  r0, 1\r\n"
		"    la r13, 0xFFFF0000\r\n"     // main never returns; it shuts the machine down
		"    strb [r13 + 0], r0\r\n"
		"    halt\r\n"
		"print_char:\r\n"                // a leaf: no frame at all
		"    la r13, 0xFF000004\r\n"
		"    strb [r13 + 0], r0\r\n"
		"    ret\r\n"
		"print_u32:\r\n"
		"    enter\r\n"
		"    sub sp, sp, 20\r\n"
		"    str [sp + 0], r8\r\n"       // r8 and r9 must survive the calls below
		"    str [sp + 4], r9\r\n"
		"    lea r8, [sp + 8]\r\n"
		"    clr r9\r\n"
		"    mov r1, r0\r\n"
		"    ifne r1, 0, .collect\r\n"
		"    li  r0, 48\r\n"
		"    call print_char\r\n"
		"    jp  .done\r\n"
		".collect:\r\n"
		"    mod  r2, r1, 10\r\n"
		"    add  r2, r2, 48\r\n"
		"    add  r3, r8, r9\r\n"
		"    strb [r3 + 0], r2\r\n"
		"    inc  r9\r\n"
		"    div  r1, r1, 10\r\n"
		"    ifne r1, 0, .collect\r\n"
		".emit:\r\n"
		"    dec  r9\r\n"
		"    add  r3, r8, r9\r\n"
		"    ldrb r0, [r3 + 0]\r\n"
		"    call print_char\r\n"
		"    ifne r9, 0, .emit\r\n"
		".done:\r\n"
		"    ldr r8, [sp + 0]\r\n"
		"    ldr r9, [sp + 4]\r\n"
		"    leave\r\n"
		"    ret\r\n"
		"factorial:\r\n"                 // recursion: n has to outlive the recursive call
		"    enter\r\n"
		"    sub sp, sp, 4\r\n"
		"    str [sp + 0], r8\r\n"
		"    mov  r8, r0\r\n"
		"    ifle r8, 1, .base\r\n"
		"    sub  r0, r8, 1\r\n"
		"    call factorial\r\n"
		"    mul  r0, r0, r8\r\n"
		"    jp   .done\r\n"
		".base:\r\n"
		"    li r0, 1\r\n"
		".done:\r\n"
		"    ldr r8, [sp + 0]\r\n"
		"    leave\r\n"
		"    ret\r\n"
		"sum6:\r\n"                      // arguments five and six arrive at [fp + 8] and [fp + 12]
		"    enter\r\n"
		"    add r0, r0, r1\r\n"
		"    add r0, r0, r2\r\n"
		"    add r0, r0, r3\r\n"
		"    ldr r1, [fp + 8]\r\n"
		"    add r0, r0, r1\r\n"
		"    ldr r1, [fp + 12]\r\n"
		"    add r0, r0, r1\r\n"
		"    leave\r\n"
		"    ret\r\n");

	CHECK(r.assembled);
	if (!r.assembled) { Registry::instance().recordFailure(r.errors); return; }
	CHECK_EQ(r.output, std::string{ "12021" });
}

TEST(pipeline, the_loader_lowers_the_stack_limit_to_the_end_of_the_image)
{
	AssembleResult a = assembleSource(
		"@data\r\n"
		"    let table: u32[64] = [1]\r\n"
		"@bss\r\n"
		"    let scratch: u32[128]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    li r0, 1\r\n"
		"    la r13, 0xFFFF0000\r\n"
		"    strb [r13 + 0], r0\r\n", "pipeline");

	CHECK(a.ok());
	if (!a.ok()) { Registry::instance().recordFailure(a.joinedErrors()); return; }

	CeresVM vm{};
	CHECK(vm.loadProgram(a.program.value()).has_value());

	const auto& h = a.program->header();
	const u32 imageEnd = static_cast<u32>(Memory::UnrestrictedSegmentStartValue) +
		h.textSize + h.rodataSize + h.dataSize + h.bssSize;

	CHECK_EQ(vm.engine().stackLimit(), imageEnd);
	// And it actually moved: the guard used to sit on the BIOS whatever the program looked like.
	CHECK(vm.engine().stackLimit() > static_cast<u32>(Memory::UnrestrictedSegmentStartValue));

	// A reset re-runs the same image, so the same ground stays guarded.
	vm.engine().reset();
	CHECK_EQ(vm.engine().stackLimit(), imageEnd);
}

// --- Addresses only the linker knows --------------------------------------------------------

TEST(pipeline, the_linker_defines_where_each_section_starts_and_ends)
{
	AssembleResult a = assembleSource(
		"@rodata\r\n"
		"    let msg: u8[4] = \"ab\"\r\n"
		"@bss\r\n"
		"    let buf: u32[8]\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    la r1, __text_end\r\n"
		"    la r2, __bss_start\r\n"
		"    la r3, __heap_start\r\n", "pipeline");

	CHECK(a.ok());
	if (!a.ok()) { Registry::instance().recordFailure(a.joinedErrors()); return; }

	auto words = a.words();
	CHECK_EQ(words.size(), 6u); // three la, two instructions each

	// Each `la` is lui+ori, so the address it built is the two immediates put back together.
	const auto addressFrom = [&](usize luiIndex) {
		return (static_cast<u32>(Instruction{ words[luiIndex] }.imm16()) << 16) |
		       static_cast<u32>(Instruction{ words[luiIndex + 1] }.imm16());
	};

	const auto& h = a.program->header();
	const u32 base = static_cast<u32>(Memory::UnrestrictedSegmentStartValue);

	CHECK_EQ(addressFrom(0), base + h.textSize);
	CHECK_EQ(addressFrom(2), base + h.textSize + h.rodataSize + h.dataSize);
	CHECK_EQ(addressFrom(4), base + h.textSize + h.rodataSize + h.dataSize + h.bssSize);

	// __heap_start is __bss_end under the name that says what it is for.
	CHECK_EQ(addressFrom(4), base + h.textSize + h.rodataSize + h.dataSize + h.bssSize);
}

TEST(pipeline, a_program_cannot_declare_a_name_the_linker_defines)
{
	AssembleResult a = assembleSource(
		"@text\r\n"
		"global main:\r\n"
		"    nop\r\n"
		"global __heap_start:\r\n"
		"    nop\r\n", "pipeline");

	CHECK(!a.ok());
	CHECK(a.joinedErrors().find("defined by the linker") != std::string::npos);
}

TEST(pipeline, a_pc_relative_load_and_store_actually_reach_the_variable)
{
	RunResult r = assembleAndRun(std::format(
		"@data\r\n"
		"    let counter: u32 = 65\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    ldvp r1, counter\r\n"
		"    inc  r1\r\n"
		"    stvp counter, r1\r\n"
		"    ldvp r2, counter\r\n"
		"    la r13, 0xFF000004\r\n"
		"    strb [r13 + 0], r2\r\n"
		"{}", shutdown));

	CHECK(r.assembled);
	if (!r.assembled) { Registry::instance().recordFailure(r.errors); return; }
	CHECK_EQ(r.output, std::string{ "B" }); // 65 read, 66 written and read back
}

TEST(pipeline, a_leaf_called_with_bl_returns_without_touching_the_stack)
{
	RunResult r = assembleAndRun(std::format(
		"@text\r\n"
		"global main:\r\n"
		"    li r0, 79\r\n"
		"    bl r11, print_char\r\n"
		"    li r0, 75\r\n"
		"    bl r11, print_char\r\n"
		"{}"
		"print_char:\r\n"
		"    la r13, 0xFF000004\r\n"
		"    strb [r13 + 0], r0\r\n"
		"    jp r11\r\n", shutdown));

	CHECK(r.assembled);
	if (!r.assembled) { Registry::instance().recordFailure(r.errors); return; }
	CHECK_EQ(r.output, std::string{ "OK" });
}

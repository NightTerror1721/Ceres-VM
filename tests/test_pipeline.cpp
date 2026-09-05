// End-to-end: assemble a program, run it, compare what it printed.
//
// Output is captured through a device rather than by redirecting stdout, so the tests stay
// independent of the process's file descriptors and can run in any order.

#include "framework.h"
#include "assemble_helper.h"
#include "vm/ceresvm.h"
#include "vm/devices.h"
#include <string>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::testing;

namespace
{
	// Stands in for TerminalDevice, collecting bytes instead of printing them.
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
		void writePortHalfword(PortNumber port, u16 value) override
		{
			writePortByte(port, static_cast<u8>(value & 0xFF));
			writePortByte(port, static_cast<u8>((value >> 8) & 0xFF));
		}
		void writePortWord(PortNumber port, u32 value) override
		{
			for (int i = 0; i < 4; ++i)
				writePortByte(port, static_cast<u8>((value >> (i * 8)) & 0xFF));
		}
		void writePort(PortNumber port, Address address, u32 size) override
		{
			if (port != default_ports::TERM_OUT || size == 0)
				return;
			auto bytes = memory().peekBytes(address, size);
			for (u8 byte : bytes)
				_output.push_back(static_cast<char>(byte));
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
		"    li r0, 1\r\n"
		"    outb 0xFF, r0\r\n";
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
		"    outm 0x01, r1, r2\r\n"
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
		"    outb 0x01, r2\r\n"
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
		"    outb 0x01, r2\r\n"
		"{}"
		"emit:\r\n"
		"    li r2, 65\r\n"
		"    outb 0x01, r2\r\n"
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
		"    stv r1, counter\r\n"
		"    ldv r2, counter\r\n"
		"    outb 0x01, r2\r\n"
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
		"    outb 0x01, r2\r\n"
		"{}", shutdown));

	CHECK(r.assembled);
	if (!r.assembled) { Registry::instance().recordFailure(r.errors); return; }
	CHECK_EQ(r.output, std::string{ "Y" });
}

TEST(pipeline, a_port_held_in_a_register_reaches_the_device)
{
	RunResult r = assembleAndRun(std::format(
		"@text\r\n"
		"global main:\r\n"
		"    li r1, 0x01\r\n"
		"    li r2, 68\r\n"
		"    outb r1, r2\r\n"
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
		"const OUT_WRITE = 0x01\r\n"
		"const EXIT_CODE = 0x01\r\n"
		"const SYS_CTRL = 0xFF\r\n"
		"\r\n"
		"@rodata\r\n"
		"    let HELLO_MSG: u8[16] = \"Hello, CeresVM!\"\r\n"
		"\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    la r1, HELLO_MSG\r\n"
		"    call print\r\n"
		"    li r0, EXIT_CODE\r\n"
		"    outb SYS_CTRL, r0\r\n"
		"    ret\r\n"
		"\r\n"
		"print:\r\n"
		"    mov r3, r1\r\n"
		"    call strlen\r\n"
		"    mov r2, r0\r\n"
		"    outm OUT_WRITE, r3, r2\r\n"
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

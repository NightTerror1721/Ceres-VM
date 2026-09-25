// The timer and alignment faults: the two things the machine could not do before.
//
// Devices used to be entirely passive, so nothing could wake a halted machine, and nothing
// depended on alignment so AlignmentFault was never raised.
//
// Every device register used to be a single-byte "port" reached through in/out; now it is an
// ordinary word at an address inside the device's own 64 KiB MMIO slot (see mmio_bus.h), reached
// through the same ldr/str family everything else uses. AT (r13) is the scratch register these
// tests build a device's base address into - literally the "assembler temporary" a real program
// would use for the same job - leaving r1-r9 free for the values a test actually inspects.

#include "framework.h"
#include "assemble_helper.h"
#include <ceres/vm/ceresvm.h>
#include <ceres/devices/devices.h>
#include <ceres/devices/storage/disk.h>
#include <ceres/devices/video/text_framebuffer.h>
#include <ceres/devices/input/gamepad.h>
#include <ceres/devices/input/keyboard.h>
#include <ceres/devices/input/mouse.h>
#include <ceres/devices/video/display.h>
#include <ceres/devices/video/blitter.h>
#include <ceres/vm/bios.h>
#include <ceres/core/format/memory_map.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <span>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::devices;
using namespace ceres::fmt;
using namespace ceres::testing;

namespace
{
	class Machine
	{
	private:
		CeresVM _vm;

	public:
		explicit Machine(std::initializer_list<Instruction> program)
		{
			const Address entry = Memory::UnrestrictedSegmentStart;

			usize offset = 0;
			for (Instruction instruction : program)
			{
				_vm.memory().writeUnchecked<u32>(entry + Address(static_cast<u32>(offset)), instruction.raw());
				offset += Instruction::Size;
			}

			BIOS bios{};
			bios.initializeMemory(_vm.memory());
			_vm.memory().writeUnchecked<u32>(0_addr, entry.value());
			_vm.engine().reset();
		}

		void step(usize count = 1)
		{
			for (usize i = 0; i < count; ++i)
				_vm.engine().step();
		}

		CeresVM& vm() noexcept { return _vm; }
		u32 reg(usize index) const { return _vm.engine().registers().getValue(index); }
		const FlagRegister& flags() const { return _vm.engine().flags(); }
		Address pc() const { return _vm.engine().programCounter(); }
		Memory& memory() { return _vm.memory(); }

		// Installs a handler at a fixed spot and points an interrupt vector at it.
		void installHandler(InterruptNumber number, Address at, std::initializer_list<Instruction> handler)
		{
			usize offset = 0;
			for (Instruction instruction : handler)
			{
				_vm.memory().writeUnchecked<u32>(at + Address(static_cast<u32>(offset)), instruction.raw());
				offset += Instruction::Size;
			}
			_vm.memory().writeUnchecked<u32>(Address(static_cast<u32>(number) * Address::Size), at.value());
		}
	};

	constexpr u32 EntryPoint = Memory::UnrestrictedSegmentStart.value();

	// Where AT (r13) is loaded from, every time a test below needs to reach a device: the two
	// words - LUI then ORI - a real program would spend to build the same 32-bit address.
	constexpr u8 Base = 13;
	constexpr u16 Hi(Address address) noexcept { return static_cast<u16>(address.value() >> 16); }
	constexpr u16 Lo(Address address) noexcept { return static_cast<u16>(address.value() & 0xFFFF); }
	Instruction LoadBase(Address address) noexcept { return Instruction::LUI(Base, Hi(address)); }
	Instruction LoadBaseLow(Address address) noexcept { return Instruction::ORI(Base, Base, Lo(address)); }

	constexpr Address TimerBase = default_mmio::Timer;
	constexpr Address TerminalBase = default_mmio::Terminal;
	constexpr Address DiskBase = default_mmio::Disk;
	constexpr Address FramebufferBase = default_mmio::Framebuffer;

	u16 Off(Address registerOffset) noexcept { return static_cast<u16>(registerOffset.value()); }
}

// --- Alignment faults --------------------------------------------------------------------------

TEST(devices, a_misaligned_word_load_raises_a_fault)
{
	// r1 = 0x401, which is not a multiple of four.
	Machine m{
		Instruction::LI(1, 0x401),
		Instruction::LDR(2, 1, 0),
	};
	m.step(2);

	// The BIOS vector for AlignmentFault points at the stub, so the PC leaves the program.
	CHECK(m.pc().value() >= Memory::BiosSegmentStart.value());
	CHECK(m.pc().value() < Memory::UnrestrictedSegmentStart.value());
}

TEST(devices, a_misaligned_halfword_store_raises_a_fault)
{
	// Well clear of the program itself, or the check below would be reading instructions.
	Machine m{
		Instruction::LI(1, 0x501),
		Instruction::LI(2, 0x1234),
		Instruction::STRH(1, 2, 0),
	};
	m.step(3);

	CHECK(m.pc().value() >= Memory::BiosSegmentStart.value());

	// And the store did not happen.
	CHECK_EQ(m.memory().readUnchecked<u16>(Address(0x501)), u16{ 0 });
}

TEST(devices, an_aligned_access_is_untouched)
{
	Machine m{
		Instruction::LI(1, 0x404),
		Instruction::LI(2, 0x1234),
		Instruction::STRH(1, 2, 0),
		Instruction::LDRH(3, 1, 0),
	};
	m.step(4);

	CHECK_EQ(m.reg(3), 0x1234u);
	CHECK_EQ(m.pc().value(), EntryPoint + 4 * Instruction::Size);
}

TEST(devices, a_byte_access_never_faults_whatever_the_address)
{
	Machine m{
		Instruction::LI(1, 0x403),
		Instruction::LI(2, 0x7F),
		Instruction::STRB(1, 2, 0),
		Instruction::LDRB(3, 1, 0),
	};
	m.step(4);

	CHECK_EQ(m.reg(3), 0x7Fu);
	CHECK_EQ(m.pc().value(), EntryPoint + 4 * Instruction::Size);
}

TEST(devices, a_displacement_can_be_what_misaligns_an_access)
{
	// The base is aligned; the displacement is not.
	Machine m{
		Instruction::LI(1, 0x400),
		Instruction::LDR(2, 1, 2),
	};
	m.step(2);

	CHECK(m.pc().value() >= Memory::BiosSegmentStart.value());
}

TEST(devices, the_interrupt_directive_installs_a_real_handler_for_terminal_input)
{
	// The whole path this feature exists for: a real .casm program, assembled and loaded exactly
	// as `ceres run` would, ends up with its own handler in the vector table instead of the BIOS's.
	AssembleResult r = assembleSource(
		"interrupt UserInterrupt1: term_isr\r\n"
		"@text\r\n"
		"global main:\r\n"
		"    sti\r\n"
		"    halt\r\n"
		"    halt\r\n" // never reached; just gives the resume point after IRET somewhere harmless
		"term_isr:\r\n"
		"    la r13, 0xFF000000\r\n" // Terminal's MMIO base
		"    ldr  r1, [r13 + 8]\r\n" // InputRegister
		"    str  [r13 + 4], r1\r\n" // OutputRegister - echo it straight back
		"    iret\r\n");

	CHECK(r.ok());
	if (!r.ok()) { Registry::instance().recordFailure(r.joinedErrors()); return; }

	CeresVM vm{};
	TerminalDevice terminal{};
	terminal.attachTo(vm.io());

	std::string captured;
	terminal.setOutputSink([&captured](u8 byte) { captured.push_back(static_cast<char>(byte)); });

	auto loaded = vm.loadProgram(r.program.value());
	CHECK(loaded.has_value());
	if (!loaded.has_value()) { Registry::instance().recordFailure(loaded.error()); return; }

	vm.engine().step(); // sti
	vm.engine().step(); // halt
	CHECK(vm.engine().flags().halting());

	terminal.pushInput('Q');

	vm.engine().step(); // delivers UserInterrupt1 and, in the same step, runs `la`
	vm.engine().step(); // ldrb
	vm.engine().step(); // strb
	vm.engine().step(); // iret

	CHECK(!vm.engine().flags().halting());
	CHECK_EQ(captured, std::string{ "Q" });
}

TEST(devices, the_engine_counts_the_instructions_it_retires)
{
	Machine m{
		Instruction::LI(1, 1),
		Instruction::LI(2, 2),
		Instruction::ADD(3, 1, 2),
	};

	CHECK_EQ(m.vm().engine().executedInstructions(), u64{ 0 });
	m.step(3);
	CHECK_EQ(m.vm().engine().executedInstructions(), u64{ 3 });

	// HALT retires like any other instruction, but the idle spins after it do not.
	m.vm().engine().reset();
	CHECK_EQ(m.vm().engine().executedInstructions(), u64{ 0 });
}

TEST(devices, a_debugger_can_write_the_machine_state_back)
{
	Machine m{ Instruction::NOP(), Instruction::NOP() };
	ExecutionEngine& engine = m.vm().engine();

	CHECK(engine.setRegister(3, 0xDEADBEEF));
	CHECK_EQ(m.reg(3), 0xDEADBEEFu);

	CHECK(engine.setFloatRegister(2, 1.5f));
	CHECK_EQ(m.vm().engine().fregisters().getValue(2), 1.5f);

	// Out of range is rejected rather than corrupting whatever sits past the array.
	CHECK(!engine.setRegister(16, 1));
	CHECK(!engine.setFloatRegister(16, 1.0f));

	const Address target = Memory::UnrestrictedSegmentStart + Address(Instruction::Size);
	engine.setProgramCounter(target);
	CHECK_EQ(m.pc().value(), target.value());
}

// --- Device registers take 32-bit accesses only (plan/v2 SPEC 5.1) -------------------------------

namespace
{
	constexpr u32 TookNothing = 0;
	constexpr u32 TookMemoryFault = 1;
	constexpr u32 TookAlignmentFault = 2;

	struct FaultSeen
	{
		u32 taken = TookNothing;
		u32 reason = 0;
	};

	// Runs `program` (`steps` instructions) with the terminal attached and a handler on each of the two faults
	// that leaves its mark in r9, and reports which one ran and the reason the engine gave.
	FaultSeen runAgainstTheTerminal(std::initializer_list<Instruction> program, usize steps)
	{
		Machine m{ program };
		m.installHandler(InterruptNumber::MemoryFault, Address(0x800), { Instruction::LI(9, TookMemoryFault), Instruction::HALT() });
		m.installHandler(InterruptNumber::AlignmentFault, Address(0x840), { Instruction::LI(9, TookAlignmentFault), Instruction::HALT() });
		TerminalDevice terminal{};
		terminal.setOutputSink([](u8) {});            // what a working store writes is not the point here
		terminal.attachTo(m.vm().io());
		m.step(steps + 2);
		const FaultSeen seen{ m.reg(9), m.vm().engine().faultReason() };
		terminal.detachFrom(m.vm().io());
		return seen;
	}

	constexpr u32 MmioWidth = static_cast<u32>(FaultReason::MmioWidth);
	constexpr u32 MmioBlock = static_cast<u32>(FaultReason::MmioBlock);
}

TEST(devices, a_byte_or_halfword_access_to_a_device_register_is_a_memory_fault)
{
	for (const Instruction narrow : { Instruction::LDRB(1, Base, 0), Instruction::LDRSB(1, Base, 0), Instruction::LDRH(1, Base, 0),
			 Instruction::LDRSH(1, Base, 0), Instruction::STRB(Base, 1, 4), Instruction::STRH(Base, 1, 4) })
	{
		const FaultSeen seen = runAgainstTheTerminal({ LoadBase(TerminalBase), LoadBaseLow(TerminalBase), narrow }, 3);
		CHECK_EQ(seen.taken, TookMemoryFault);
		CHECK_EQ(seen.reason, MmioWidth);
	}
}

TEST(devices, a_misaligned_word_access_to_a_device_is_an_alignment_fault_of_width)
{
	const FaultSeen seen = runAgainstTheTerminal({ LoadBase(TerminalBase), LoadBaseLow(TerminalBase), Instruction::LDR(1, Base, 2) }, 3);
	CHECK_EQ(seen.taken, TookAlignmentFault);
	CHECK_EQ(seen.reason, MmioWidth);
}

TEST(devices, a_misaligned_word_access_to_ram_says_alignment)
{
	const FaultSeen seen = runAgainstTheTerminal({ Instruction::LI(1, 0x601), Instruction::LDR(2, 1, 0) }, 2);
	CHECK_EQ(seen.taken, TookAlignmentFault);
	CHECK_EQ(seen.reason, static_cast<u32>(FaultReason::Alignment));
}

TEST(devices, a_word_access_to_a_device_register_still_works)
{
	const FaultSeen seen = runAgainstTheTerminal({ LoadBase(TerminalBase), LoadBaseLow(TerminalBase),
		Instruction::LDR(1, Base, Off(TerminalDevice::StatusRegister)), Instruction::LI(2, 'w'),
		Instruction::STR(Base, 2, Off(TerminalDevice::OutputRegister)) }, 5);
	CHECK_EQ(seen.taken, TookNothing);
	CHECK_EQ(seen.reason, 0u);
}

TEST(devices, every_device_declares_a_sound_register_table)
{
	TerminalDevice terminal{};
	TimerDevice timer{};
	SystemControlDevice control{};
	DmaController dma{};
	DiskDevice disk{ 4 };
	FramebufferDevice framebuffer{};
	KeyboardDevice keyboard{};
	MouseDevice mouse{};
	DisplayDevice display{};
	GamepadDevice gamepad{};
	AudioDevice audio{};
	PeripheralDevice peripherals{};
	HostFsDevice hostFs{};
	BlitterDevice blitter{};
	const std::array<const IODevice*, 14> devices{ &terminal, &timer, &control, &dma, &disk, &framebuffer, &keyboard, &mouse,
		&display, &gamepad, &audio, &peripherals, &hostFs, &blitter };

	std::vector<std::string_view> names;
	for (const IODevice* device : devices)
	{
		const RegisterMap& map = device->registers();
		CHECK(!map.device().empty());
		CHECK(!map.empty());
		names.push_back(map.device());
		u32 previous = 0;
		bool first = true;
		for (const RegisterInfo& info : map.registers())
		{
			CHECK_EQ(info.offset % 4, 0u);                    // a register is a whole aligned word
			CHECK(info.offset < 0x100);                        // what the bus's per-slot mask covers
			CHECK(first || info.offset > previous);            // in order, so no offset is declared twice
			CHECK(!info.name.empty());
			CHECK(!info.description.empty());
			CHECK(map.find(info.offset) == &info);
			previous = info.offset;
			first = false;
		}
	}
	std::ranges::sort(names);
	CHECK(std::ranges::adjacent_find(names) == names.end());  // `dev <name>` finds one device
}

TEST(devices, an_undeclared_offset_reads_zero_and_under_strict_mmio_faults)
{
	// 0x40 is no register of the terminal's. Through the bus it reads 0 (and a write is dropped) unless the
	// machine is strict, when it is a MemoryFault of its own reason.
	const FaultSeen lenient = runAgainstTheTerminal({ LoadBase(TerminalBase), LoadBaseLow(TerminalBase),
		Instruction::LI(1, 7), Instruction::LDR(1, Base, 0x40), Instruction::STR(Base, 1, 0x40) }, 5);
	CHECK_EQ(lenient.taken, TookNothing);

	Machine m{ LoadBase(TerminalBase), LoadBaseLow(TerminalBase), Instruction::LI(1, 7), Instruction::LDR(1, Base, 0x40) };
	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());
	m.step(4);
	CHECK_EQ(m.reg(1), 0u);

	Machine strict{ LoadBase(TerminalBase), LoadBaseLow(TerminalBase), Instruction::LDR(1, Base, 0x40) };
	strict.installHandler(InterruptNumber::MemoryFault, Address(0x800), { Instruction::LI(9, TookMemoryFault), Instruction::HALT() });
	strict.vm().engine().setStrictMmio(true);
	TerminalDevice strictTerminal{};
	strictTerminal.attachTo(strict.vm().io());
	strict.step(5);
	CHECK_EQ(strict.reg(9), TookMemoryFault);
	CHECK_EQ(strict.vm().engine().faultReason(), static_cast<u32>(FaultReason::MmioUndeclared));
	strictTerminal.detachFrom(strict.vm().io());
	terminal.detachFrom(m.vm().io());
}

TEST(devices, a_block_instruction_that_touches_a_device_is_a_memory_fault)
{
	// r2 a buffer in RAM, r3 a count, r4 a byte; Base (r13) the terminal.
	for (const Instruction block : { Instruction::MCPY(Base, 2, 3), Instruction::MCPY(2, Base, 3), Instruction::MSET(Base, 4, 3),
			 Instruction::MCMP(Base, 2, 3), Instruction::MSCAN(Base, 4, 3) })
	{
		const FaultSeen seen = runAgainstTheTerminal({ LoadBase(TerminalBase), LoadBaseLow(TerminalBase),
			Instruction::LI(2, 0x600), Instruction::LI(3, 4), Instruction::LI(4, 0x41), block }, 6);
		CHECK_EQ(seen.taken, TookMemoryFault);
		CHECK_EQ(seen.reason, MmioBlock);
	}
}

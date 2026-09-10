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
#include <ceres/devices/storage_devices.h>
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

// --- The timer ---------------------------------------------------------------------------------

TEST(devices, the_tick_port_counts_executed_instructions)
{
	Machine m{
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::NOP(),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());

	m.step(3);

	CHECK_EQ(timer.ticks(), u64{ 3 });
}

TEST(devices, an_armed_timer_raises_its_interrupt)
{
	Machine m{
		Instruction::STI(),
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::NOP(),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(3);

	// A handler that just marks a register and returns.
	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0xABC),
		Instruction::IRET(),
	});

	m.step(8);

	CHECK_EQ(m.reg(9), 0xABCu);
	CHECK(!timer.isArmed());
}

TEST(devices, a_masked_timer_interrupt_stays_pending_until_interrupts_are_enabled)
{
	// Without STI the request must be held, not thrown away: user interrupts are masked while
	// the interrupt flag is clear.
	Machine m{
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::NOP(),
		Instruction::STI(),
		Instruction::NOP(),
		Instruction::NOP(),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(2);

	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x5A),
		Instruction::IRET(),
	});

	// After three steps the timer has expired but the flag is still clear.
	m.step(3);
	CHECK_EQ(m.reg(9), 0u);
	CHECK(m.vm().interrupts().hasPending());

	// STI, and the queued request is delivered.
	m.step(4);
	CHECK_EQ(m.reg(9), 0x5Au);
}

TEST(devices, the_timer_wakes_a_halted_machine)
{
	// This is what the timer is for. HALT used to suspend the machine for good, because no device
	// could ever speak first.
	Machine m{
		Instruction::STI(),
		Instruction::HALT(),
		Instruction::LI(9, 0x77),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(4);

	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::IRET(),
	});

	m.step(2);
	CHECK(m.flags().halting());

	// Stepping on, the timer expires and the interrupt clears the halting flag.
	m.step(12);

	CHECK(!m.flags().halting());
	CHECK_EQ(m.reg(9), 0x77u);
}

TEST(devices, a_periodic_timer_re_arms_itself)
{
	Machine m{
		Instruction::STI(),
		Instruction::NOP(), Instruction::NOP(), Instruction::NOP(), Instruction::NOP(),
		Instruction::NOP(), Instruction::NOP(), Instruction::NOP(), Instruction::NOP(),
		Instruction::NOP(), Instruction::NOP(), Instruction::NOP(), Instruction::NOP(),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(2, true);

	// The handler counts how many times it ran.
	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::ADDI(9, 9, 1),
		Instruction::IRET(),
	});

	m.step(20);

	CHECK(m.reg(9) >= 2u);
	CHECK(timer.isArmed());
}

TEST(devices, a_program_can_arm_the_timer_through_its_register)
{
	Machine m{
		Instruction::STI(),
		Instruction::LI(1, 3),
		LoadBase(TimerBase), LoadBaseLow(TimerBase),
		Instruction::STR(Base, 1, Off(TimerDevice::CommandRegister)),
		Instruction::NOP(), Instruction::NOP(), Instruction::NOP(), Instruction::NOP(),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());

	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x33),
		Instruction::IRET(),
	});

	m.step(14);

	CHECK_EQ(m.reg(9), 0x33u);
}

TEST(devices, writing_zero_disarms_the_timer)
{
	Machine m{ Instruction::NOP() };

	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	timer.arm(5);
	CHECK(timer.isArmed());

	timer.arm(0);
	CHECK(!timer.isArmed());
}

// --- The terminal's input interrupt -------------------------------------------------------------

TEST(devices, pushing_input_raises_the_terminals_interrupt)
{
	Machine m{ Instruction::STI(), Instruction::NOP(), Instruction::NOP() };

	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());

	m.installHandler(TerminalDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x51),
		Instruction::IRET(),
	});

	terminal.pushInput('X');
	m.step(3);

	CHECK_EQ(m.reg(9), 0x51u);
}

TEST(devices, the_terminal_wakes_a_halted_machine_on_input)
{
	// The motivating case: HALT suspends the machine until *some* interrupt arrives, and the
	// terminal is now one of the things that can raise one - not just the timer.
	Machine m{
		Instruction::STI(),
		Instruction::HALT(),
		LoadBase(TerminalBase), LoadBaseLow(TerminalBase),
		Instruction::LDRB(1, Base, Off(TerminalDevice::InputRegister)),
	};

	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());

	m.installHandler(TerminalDevice::Interrupt, Address(0x800), {
		Instruction::IRET(),
	});

	m.step(2);
	CHECK(m.flags().halting());

	// Nothing was pending before this: the interrupt is raised by the push itself, not by time
	// passing, which is exactly the difference from the timer.
	terminal.pushInput('X');
	m.step(4);

	CHECK(!m.flags().halting());
	CHECK_EQ(m.reg(1), static_cast<u32>('X'));
}

TEST(devices, pushing_input_with_nothing_to_deliver_raises_no_interrupt)
{
	// An empty span (or a full ring buffer) writes no byte, so there is nothing to be woken up
	// about - and nothing here to distinguish from the interrupt never having been requested.
	Machine m{ Instruction::STI(), Instruction::HALT() };

	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());

	m.step(2);
	CHECK(m.flags().halting());

	terminal.pushInput(std::span<const u8>{});
	m.step(1);

	CHECK(m.flags().halting());
}

TEST(devices, terminal_counts_input_discarded_by_a_full_buffer)
{
	TerminalDevice terminal{};
	std::vector<u8> input(TerminalDevice::InputBufferCapacity + 8, 'X');

	terminal.pushInput(input);

	CHECK_EQ(terminal.droppedInputBytes(), u64{9});
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
		"    ldrb r1, [r13 + 8]\r\n" // InputRegister
		"    strb [r13 + 4], r1\r\n" // OutputRegister - echo it straight back
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

// --- The terminal's output sink ----------------------------------------------------------------

TEST(devices, terminal_output_goes_to_the_installed_sink_instead_of_stdout)
{
	// A byte write and a word write, low byte first: together they cover both paths through
	// emitByte.
	Machine m{
		LoadBase(TerminalBase), LoadBaseLow(TerminalBase),
		Instruction::LI(1, 'C'),
		Instruction::STRB(Base, 1, Off(TerminalDevice::OutputRegister)),
		Instruction::LI(2, 0x0000'6165), // 'e', 'a', 0, 0
		Instruction::STR(Base, 2, Off(TerminalDevice::OutputRegister)),
	};

	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());

	std::string captured;
	terminal.setOutputSink([&captured](u8 byte) { captured.push_back(static_cast<char>(byte)); });

	m.step(6);

	CHECK_EQ(captured.size(), usize{ 5 });
	CHECK(captured.starts_with("Cea"));
}

TEST(devices, a_multi_byte_character_reaches_the_sink_one_byte_at_a_time)
{
	// 'á' is 0xC3 0xA1 in UTF-8. The device hands over bytes, not characters: a consumer that
	// decoded each one on its own would produce two replacement characters instead of one letter.
	Machine m{
		LoadBase(TerminalBase), LoadBaseLow(TerminalBase),
		Instruction::LI(1, 0xC3),
		Instruction::STRB(Base, 1, Off(TerminalDevice::OutputRegister)),
		Instruction::LI(1, 0xA1),
		Instruction::STRB(Base, 1, Off(TerminalDevice::OutputRegister)),
	};

	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());

	std::vector<u8> captured;
	terminal.setOutputSink([&captured](u8 byte) { captured.push_back(byte); });

	m.step(6);

	CHECK_EQ(captured.size(), usize{ 2 });
	if (captured.size() == 2)
	{
		CHECK_EQ(captured[0], u8{ 0xC3 });
		CHECK_EQ(captured[1], u8{ 0xA1 });
	}
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

// --- The two device ranges that were reserved and empty -------------------------------------------
//
// The disk and framebuffer slots were reserved in the MMIO map from the start and answered
// nothing. A program could read an unattached device's registers and get 0xFFFFFFFF back, which
// is what an absent device looks like.

namespace
{
	// Somewhere well clear of the handful of instructions each test loads at the start of the
	// unrestricted segment.
	constexpr u32 SourceBuffer = 0x1000;
	constexpr u32 DestinationBuffer = 0x2000;

	void fill(Memory& memory, u32 address, std::string_view bytes)
	{
		for (u32 i = 0; i < bytes.size(); ++i)
			memory.writeUnchecked<u8>(Address(address + i), static_cast<u8>(bytes[i]));
	}

	std::string readBack(Memory& memory, u32 address, u32 size)
	{
		std::string out;
		for (u32 i = 0; i < size; ++i)
			out.push_back(static_cast<char>(memory.readUnchecked<u8>(Address(address + i))));
		return out;
	}
}

TEST(devices, a_sector_written_to_the_disk_comes_back_the_same)
{
	Machine m{
		LoadBase(DiskBase), LoadBaseLow(DiskBase),
		Instruction::LI(1, 1),
		Instruction::STR(Base, 1, Off(DiskDevice::SectorRegister)),
		Instruction::LI(2, static_cast<u16>(SourceBuffer)),
		Instruction::LI(3, 8),
		Instruction::STR(Base, 2, Off(DiskDevice::BlockAddressRegister)),
		Instruction::STR(Base, 3, Off(DiskDevice::BlockLengthRegister)),
		Instruction::LI(5, DiskDevice::BlockCommandWrite),
		Instruction::STR(Base, 5, Off(DiskDevice::BlockCommandRegister)), // memory -> sector 1
		Instruction::LI(4, static_cast<u16>(DestinationBuffer)),
		Instruction::STR(Base, 4, Off(DiskDevice::BlockAddressRegister)),
		Instruction::LI(5, DiskDevice::BlockCommandRead),
		Instruction::STR(Base, 5, Off(DiskDevice::BlockCommandRegister)), // sector 1 -> memory, somewhere else
	};

	DiskDevice disk{};
	disk.attachTo(m.vm().io());

	fill(m.memory(), SourceBuffer, "ON A DISK");
	m.step(16);

	CHECK_EQ(readBack(m.memory(), DestinationBuffer, 8), std::string{ "ON A DIS" });

	// And the sector it did not select is untouched, which is the whole reason for selecting one.
	CHECK_EQ(disk.image()[0], u8{ 0 });
}

TEST(devices, a_sector_past_the_end_of_the_disk_is_refused_rather_than_wrapped)
{
	Machine m{
		LoadBase(DiskBase), LoadBaseLow(DiskBase),
		Instruction::LI(1, 9999),
		Instruction::STR(Base, 1, Off(DiskDevice::SectorRegister)),
		Instruction::LI(2, static_cast<u16>(SourceBuffer)),
		Instruction::LI(3, 8),
		Instruction::STR(Base, 2, Off(DiskDevice::BlockAddressRegister)),
		Instruction::STR(Base, 3, Off(DiskDevice::BlockLengthRegister)),
		Instruction::LI(5, DiskDevice::BlockCommandWrite),
		Instruction::STR(Base, 5, Off(DiskDevice::BlockCommandRegister)),
		Instruction::LDRB(6, Base, Off(DiskDevice::StatusRegister)),
	};

	DiskDevice disk{ 4 }; // Four sectors, so 9999 is nowhere
	disk.attachTo(m.vm().io());

	fill(m.memory(), SourceBuffer, "NOWHERE");
	m.step(13);

	CHECK_EQ(m.reg(6) & DiskDevice::StatusError, DiskDevice::StatusError);
	CHECK_EQ(m.reg(6) & DiskDevice::StatusReady, 0u);
}

TEST(devices, a_file_backed_disk_still_holds_what_was_written_after_the_machine_stops)
{
	const auto path = std::filesystem::temp_directory_path() / "ceres_test_disk.img";
	std::filesystem::remove(path);

	{
		Machine m{
			LoadBase(DiskBase), LoadBaseLow(DiskBase),
			Instruction::LI(1, 2),
			Instruction::STR(Base, 1, Off(DiskDevice::SectorRegister)),
			Instruction::LI(2, static_cast<u16>(SourceBuffer)),
			Instruction::LI(3, 6),
			Instruction::STR(Base, 2, Off(DiskDevice::BlockAddressRegister)),
			Instruction::STR(Base, 3, Off(DiskDevice::BlockLengthRegister)),
			Instruction::LI(5, DiskDevice::BlockCommandWrite),
			Instruction::STR(Base, 5, Off(DiskDevice::BlockCommandRegister)),
			Instruction::LI(4, DiskDevice::CommandFlush),
			Instruction::STR(Base, 4, Off(DiskDevice::CommandRegister)),
		};

		DiskDevice disk{};
		CHECK(disk.open(path, 8));
		disk.attachTo(m.vm().io());

		fill(m.memory(), SourceBuffer, "PERSIST");
		m.step(14);
	}

	// A second machine, a second device, the same file.
	Machine m{
		LoadBase(DiskBase), LoadBaseLow(DiskBase),
		Instruction::LI(1, 2),
		Instruction::STR(Base, 1, Off(DiskDevice::SectorRegister)),
		Instruction::LI(2, static_cast<u16>(DestinationBuffer)),
		Instruction::LI(3, 6),
		Instruction::STR(Base, 2, Off(DiskDevice::BlockAddressRegister)),
		Instruction::STR(Base, 3, Off(DiskDevice::BlockLengthRegister)),
		Instruction::LI(5, DiskDevice::BlockCommandRead),
		Instruction::STR(Base, 5, Off(DiskDevice::BlockCommandRegister)),
	};

	DiskDevice disk{};
	CHECK(disk.open(path, 8));
	disk.attachTo(m.vm().io());
	m.step(12);

	CHECK_EQ(readBack(m.memory(), DestinationBuffer, 6), std::string{ "PERSIS" });
	std::filesystem::remove(path);
}

TEST(devices, the_framebuffer_shows_the_grid_it_was_given)
{
	Machine m{
		LoadBase(FramebufferBase), LoadBaseLow(FramebufferBase),
		Instruction::LI(1, 4),
		Instruction::STR(Base, 1, Off(FramebufferDevice::WidthRegister)),
		Instruction::LI(1, 2),
		Instruction::STR(Base, 1, Off(FramebufferDevice::HeightRegister)),
		Instruction::LI(1, FramebufferDevice::CommandClear),
		Instruction::STR(Base, 1, Off(FramebufferDevice::CommandRegister)),
		Instruction::LI(2, static_cast<u16>(SourceBuffer)),
		Instruction::LI(3, 8),
		Instruction::STR(Base, 2, Off(FramebufferDevice::BlockAddressRegister)),
		Instruction::STR(Base, 3, Off(FramebufferDevice::BlockLengthRegister)),
		Instruction::LI(4, FramebufferDevice::BlockCommandWrite),
		Instruction::STR(Base, 4, Off(FramebufferDevice::BlockCommandRegister)),
		Instruction::LI(1, FramebufferDevice::CommandPresent),
		Instruction::STR(Base, 1, Off(FramebufferDevice::CommandRegister)),
	};

	FramebufferDevice framebuffer{};
	framebuffer.attachTo(m.vm().io());

	std::string shown;
	framebuffer.setPresentSink([&shown](std::string_view frame) { shown = frame; });

	fill(m.memory(), SourceBuffer, "ab..#..#");
	m.step(18);

	// Rows, not a stream: the second four cells are the second line.
	CHECK_EQ(shown, std::string{ "ab..\n#..#\n" });
}

TEST(devices, a_cell_that_would_move_the_terminals_own_cursor_is_shown_as_a_space)
{
	Machine m{ Instruction::NOP() };

	FramebufferDevice framebuffer{};
	framebuffer.attachTo(m.vm().io());

	framebuffer.writeWord(FramebufferDevice::WidthRegister, 3);
	framebuffer.writeWord(FramebufferDevice::HeightRegister, 1);
	framebuffer.writeWord(FramebufferDevice::DataRegister, 'x');
	framebuffer.writeWord(FramebufferDevice::DataRegister, 0x07); // a bell
	framebuffer.writeWord(FramebufferDevice::DataRegister, 'y');

	CHECK_EQ(framebuffer.toText(), std::string{ "x y\n" });
}

TEST(devices, a_grid_larger_than_any_terminal_is_a_typo_and_is_ignored)
{
	Machine m{ Instruction::NOP() };

	FramebufferDevice framebuffer{};
	framebuffer.attachTo(m.vm().io());

	const u32 before = framebuffer.width();
	framebuffer.writeWord(FramebufferDevice::WidthRegister, 100000);
	CHECK_EQ(framebuffer.width(), before);

	framebuffer.writeWord(FramebufferDevice::HeightRegister, 0);
	CHECK_EQ(framebuffer.height(), u32{ 20 });
}

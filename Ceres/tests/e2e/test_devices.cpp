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
		Instruction::LDR(1, Base, Off(TerminalDevice::InputRegister)),
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

TEST(devices, the_terminal_reports_how_many_bytes_are_available)
{
	TerminalDevice terminal{};

	CHECK_EQ(terminal.availableBytes(), usize{ 0 });

	terminal.pushInput("AB");
	CHECK_EQ(terminal.availableBytes(), usize{ 2 });
	CHECK_EQ(terminal.readUnsignedWord(TerminalDevice::BytesAvailableRegister), u32{ 2 });

	terminal.readUnsignedByte(TerminalDevice::InputRegister);
	CHECK_EQ(terminal.readUnsignedWord(TerminalDevice::BytesAvailableRegister), u32{ 1 });
}

TEST(devices, a_block_read_reports_how_many_bytes_it_actually_moved)
{
	// Five bytes buffered, ten asked for: the read is short, and the count register is the only
	// way the program can tell where its input ended.
	CeresVM vm{};
	TerminalDevice terminal{};
	terminal.attachTo(vm.io());
	terminal.pushInput("ABCDE");

	terminal.writeWord(TerminalDevice::BlockAddressRegister, 0x1000);
	terminal.writeWord(TerminalDevice::BlockLengthRegister, 10);
	terminal.writeWord(TerminalDevice::BlockCommandRegister, TerminalDevice::BlockCommandRead);

	CHECK_EQ(terminal.readUnsignedWord(TerminalDevice::BlockReadCountRegister), u32{ 5 });
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(0x1000)), u8{ 'A' });
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(0x1004)), u8{ 'E' });
	CHECK_EQ(terminal.availableBytes(), usize{ 0 });
}

TEST(devices, a_zero_length_block_read_reports_nothing_moved)
{
	CeresVM vm{};
	TerminalDevice terminal{};
	terminal.attachTo(vm.io());
	terminal.pushInput("X");

	terminal.writeWord(TerminalDevice::BlockAddressRegister, 0x1000);
	terminal.writeWord(TerminalDevice::BlockLengthRegister, 0);
	terminal.writeWord(TerminalDevice::BlockCommandRegister, TerminalDevice::BlockCommandRead);

	CHECK_EQ(terminal.readUnsignedWord(TerminalDevice::BlockReadCountRegister), u32{ 0 });
	CHECK_EQ(terminal.availableBytes(), usize{ 1 }); // Nothing was consumed.
}

TEST(devices, a_block_read_to_an_out_of_range_address_moves_nothing_instead_of_throwing)
{
	// A program-supplied BLOCK_ADDR past the end of RAM must not crash the machine: the read is
	// simply empty, and the count register says so.
	CeresVM vm{};
	TerminalDevice terminal{};
	terminal.attachTo(vm.io());
	terminal.pushInput("ABCDE");

	terminal.writeWord(TerminalDevice::BlockAddressRegister, 0xF0000000u);
	terminal.writeWord(TerminalDevice::BlockLengthRegister, 5);
	terminal.writeWord(TerminalDevice::BlockCommandRegister, TerminalDevice::BlockCommandRead);

	CHECK_EQ(terminal.readUnsignedWord(TerminalDevice::BlockReadCountRegister), u32{ 0 });
	CHECK_EQ(terminal.availableBytes(), usize{ 5 }); // Nothing was consumed.
}

TEST(devices, a_block_read_is_clamped_to_the_end_of_ram)
{
	// Four bytes of RAM left, ten bytes buffered: the transfer stops at the end of memory and the
	// count reports the four it actually moved.
	CeresVM vm{}; // 16 MiB default
	TerminalDevice terminal{};
	terminal.attachTo(vm.io());
	terminal.pushInput("ABCDEFGHIJ");

	const u32 lastWord = static_cast<u32>(vm.memory().size()) - 4u;
	terminal.writeWord(TerminalDevice::BlockAddressRegister, lastWord);
	terminal.writeWord(TerminalDevice::BlockLengthRegister, 100);
	terminal.writeWord(TerminalDevice::BlockCommandRegister, TerminalDevice::BlockCommandRead);

	CHECK_EQ(terminal.readUnsignedWord(TerminalDevice::BlockReadCountRegister), u32{ 4 });
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(lastWord)), u8{ 'A' });
	CHECK_EQ(terminal.availableBytes(), usize{ 6 });
}

TEST(devices, the_terminal_reports_dropped_input_bytes_to_the_program)
{
	TerminalDevice terminal{};
	std::vector<u8> input(TerminalDevice::InputBufferCapacity + 8, 'X');

	terminal.pushInput(input);

	CHECK_EQ(terminal.readUnsignedWord(TerminalDevice::DroppedInputRegister), u32{ 9 });
}

TEST(devices, terminal_snapshot_and_restore_preserve_unread_input)
{
	TerminalDevice terminal{};
	terminal.pushInput("AB");

	const auto snapshot = terminal.captureState();
	CHECK_EQ(terminal.readUnsignedByte(TerminalDevice::InputRegister), static_cast<u8>('A'));

	terminal.restoreState(snapshot);
	CHECK_EQ(terminal.readUnsignedByte(TerminalDevice::InputRegister), static_cast<u8>('A'));
	CHECK_EQ(terminal.readUnsignedByte(TerminalDevice::InputRegister), static_cast<u8>('B'));
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

// --- The terminal's output sink ----------------------------------------------------------------

TEST(devices, terminal_output_goes_to_the_installed_sink_instead_of_stdout)
{
	// Two word writes: each sends its low byte, and only that - the rest of the word is not more text.
	Machine m{
		LoadBase(TerminalBase), LoadBaseLow(TerminalBase),
		Instruction::LI(1, 'C'),
		Instruction::STR(Base, 1, Off(TerminalDevice::OutputRegister)),
		Instruction::LI(2, 0x0000'6165), // 'e' in the low byte, 'a' above it
		Instruction::STR(Base, 2, Off(TerminalDevice::OutputRegister)),
	};

	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());

	std::string captured;
	terminal.setOutputSink([&captured](u8 byte) { captured.push_back(static_cast<char>(byte)); });

	m.step(6);

	CHECK_EQ(captured, std::string{ "Ce" });
}

TEST(devices, a_multi_byte_character_reaches_the_sink_one_byte_at_a_time)
{
	// 'á' is 0xC3 0xA1 in UTF-8. The device hands over bytes, not characters: a consumer that
	// decoded each one on its own would produce two replacement characters instead of one letter.
	Machine m{
		LoadBase(TerminalBase), LoadBaseLow(TerminalBase),
		Instruction::LI(1, 0xC3),
		Instruction::STR(Base, 1, Off(TerminalDevice::OutputRegister)),
		Instruction::LI(1, 0xA1),
		Instruction::STR(Base, 1, Off(TerminalDevice::OutputRegister)),
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
		Instruction::LDR(6, Base, Off(DiskDevice::StatusRegister)),
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

TEST(devices, a_latin1_cell_is_shown_as_its_code_point_in_utf8)
{
	Machine m{ Instruction::NOP() };

	FramebufferDevice framebuffer{};
	framebuffer.attachTo(m.vm().io());

	framebuffer.writeWord(FramebufferDevice::WidthRegister, 4);
	framebuffer.writeWord(FramebufferDevice::HeightRegister, 1);
	framebuffer.writeWord(FramebufferDevice::DataRegister, 0xF1);   // n tilde
	framebuffer.writeWord(FramebufferDevice::DataRegister, 'o');
	framebuffer.writeWord(FramebufferDevice::DataRegister, 0x85);   // a C1 control: a space
	framebuffer.writeWord(FramebufferDevice::DataRegister, 0xFF);   // y diaeresis

	CHECK_EQ(framebuffer.toText(), std::string{ "\xC3\xB1o \xC3\xBF\n" });
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

// --- The DMA controller's transfer count -------------------------------------------------------

TEST(devices, a_dma_transfer_reports_how_many_bytes_it_moved)
{
	Machine m{ Instruction::NOP() };

	DmaController dma{};
	dma.attachTo(m.vm().io());

	fill(m.memory(), SourceBuffer, "HELLO");

	dma.writeWord(DmaController::SourceRegister, SourceBuffer);
	dma.writeWord(DmaController::DestinationRegister, DestinationBuffer);
	dma.writeWord(DmaController::LengthRegister, 5);
	dma.writeWord(DmaController::CommandRegister, DmaController::CommandStart);

	CHECK_EQ(dma.readUnsignedWord(DmaController::StatusRegister) & DmaController::StatusBusy, DmaController::StatusBusy);

	// The copy lands on the next tick, not on the arming instruction itself.
	m.step(1);

	CHECK_EQ(dma.readUnsignedWord(DmaController::StatusRegister) & DmaController::StatusDone, DmaController::StatusDone);
	CHECK_EQ(dma.readUnsignedWord(DmaController::TransferredRegister), u32{ 5 });
	CHECK_EQ(readBack(m.memory(), DestinationBuffer, 5), std::string{ "HELLO" });
}

TEST(devices, a_dma_transfer_past_the_end_of_memory_is_clamped_not_fatal)
{
	Machine m{ Instruction::NOP() };

	DmaController dma{};
	dma.attachTo(m.vm().io());

	// The source sits in the last four bytes of RAM, but 100 are asked for: the copy is clamped to 4.
	const u32 lastBytes = static_cast<u32>(m.memory().size()) - 4u;
	fill(m.memory(), lastBytes, "WXYZ");

	dma.writeWord(DmaController::SourceRegister, lastBytes);
	dma.writeWord(DmaController::DestinationRegister, DestinationBuffer);
	dma.writeWord(DmaController::LengthRegister, 100);
	dma.writeWord(DmaController::CommandRegister, DmaController::CommandStart);

	m.step(1);

	CHECK_EQ(dma.readUnsignedWord(DmaController::StatusRegister) & DmaController::StatusDone, DmaController::StatusDone);
	CHECK_EQ(dma.readUnsignedWord(DmaController::TransferredRegister), u32{ 4 });
	CHECK_EQ(readBack(m.memory(), DestinationBuffer, 4), std::string{ "WXYZ" });
}

// --- The keyboard ------------------------------------------------------------------------------

TEST(devices, the_keyboard_reports_press_and_release_events)
{
	KeyboardDevice keyboard{};

	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::StatusRegister), u32{ 0 });

	keyboard.pushKey('A', true);
	keyboard.pushKey('A', false);

	// Bits 30:0 are the code, bit 31 is the pressed flag.
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::EventRegister), u32{ 'A' | KeyboardDevice::EventPressed });
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::EventRegister), u32{ 'A' });
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::EventRegister), u32{ 0 }); // Empty again.
}

TEST(devices, the_keyboard_reports_how_many_events_are_available)
{
	KeyboardDevice keyboard{};

	CHECK_EQ(keyboard.availableEvents(), usize{ 0 });
	keyboard.pushKey('a');
	keyboard.pushKey('b');

	CHECK_EQ(keyboard.availableEvents(), usize{ 2 });
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::StatusRegister) & KeyboardDevice::StatusDataReady, KeyboardDevice::StatusDataReady);

	keyboard.readUnsignedWord(KeyboardDevice::EventRegister);
	CHECK_EQ(keyboard.availableEvents(), usize{ 1 });
}

TEST(devices, a_keyboard_block_read_drains_events_and_counts_them)
{
	CeresVM vm{};
	KeyboardDevice keyboard{};
	keyboard.attachTo(vm.io());

	keyboard.pushKey('a');
	keyboard.pushKey('b');
	keyboard.pushKey('c');

	keyboard.writeWord(KeyboardDevice::BlockAddressRegister, 0x3000);
	keyboard.writeWord(KeyboardDevice::BlockLengthRegister, 8); // room for two 4-byte events
	keyboard.writeWord(KeyboardDevice::BlockCommandRegister, KeyboardDevice::BlockCommandRead);

	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::BlockReadCountRegister), u32{ 2 });
	// One 32-bit event per four bytes, little-endian: 'a' pressed is 0x80000061.
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(0x3000)), u8{ 0x61 });
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(0x3001)), u8{ 0x00 });
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(0x3002)), u8{ 0x00 });
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(0x3003)), u8{ 0x80 });
	CHECK_EQ(vm.memory().readUnchecked<u8>(Address(0x3004)), u8{ 0x62 });
	CHECK_EQ(keyboard.availableEvents(), usize{ 1 }); // 'c' remains.
}

TEST(devices, a_keyboard_event_keeps_a_code_wider_than_eight_bits)
{
	KeyboardDevice keyboard{};

	// SDL scancodes go past 255; the code must survive round-trip in the low 31 bits.
	keyboard.pushKey(0x12345678u, true);
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::EventRegister), u32{ 0x12345678u | KeyboardDevice::EventPressed });

	keyboard.pushKey(0x12345678u, false);
	CHECK_EQ(keyboard.readUnsignedWord(KeyboardDevice::EventRegister), u32{ 0x12345678u });
}

TEST(devices, pushing_a_key_raises_the_keyboards_interrupt)
{
	Machine m{ Instruction::STI(), Instruction::NOP(), Instruction::NOP() };

	KeyboardDevice keyboard{};
	keyboard.attachTo(m.vm().io());

	m.installHandler(KeyboardDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x61),
		Instruction::IRET(),
	});

	keyboard.pushKey('X');
	m.step(3);

	CHECK_EQ(m.reg(9), 0x61u);
}

// --- The mouse ---------------------------------------------------------------------------------

TEST(devices, the_mouse_reports_deltas_and_absolute_position)
{
	MouseDevice mouse{};

	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::StatusRegister), u32{ 0 });

	mouse.pushMotion(5, -3, MouseDevice::ButtonLeft, 0);

	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::StatusRegister), MouseDevice::StatusDataReady);
	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::DeltaXRegister), u32{ 5 });
	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::DeltaYRegister), static_cast<u32>(-3));
	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::XRegister), u32{ 5 });
	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::YRegister), static_cast<u32>(-3));
	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::ButtonsRegister), u32{ MouseDevice::ButtonLeft });
}

TEST(devices, mouse_deltas_are_consumed_on_read_but_position_is_not)
{
	MouseDevice mouse{};

	mouse.pushMotion(2, 2);

	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::DeltaXRegister), u32{ 2 });
	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::DeltaXRegister), u32{ 0 }); // Consumed.
	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::XRegister), u32{ 2 });      // Absolute persists.
}

TEST(devices, the_mouse_accumulates_motion_across_push_calls)
{
	MouseDevice mouse{};

	mouse.pushMotion(1, 1);
	mouse.pushMotion(2, 2);

	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::DeltaXRegister), u32{ 3 }); // Accumulated delta.
	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::XRegister), u32{ 3 });      // Accumulated position.
}

TEST(devices, the_mouse_reports_buttons_and_wheel)
{
	MouseDevice mouse{};

	mouse.pushMotion(0, 0, MouseDevice::ButtonRight | MouseDevice::ButtonMiddle, 2);

	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::ButtonsRegister), u32{ MouseDevice::ButtonRight | MouseDevice::ButtonMiddle });
	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::WheelRegister), u32{ 2 });
	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::WheelRegister), u32{ 0 }); // Consumed.
}

TEST(devices, a_mouse_push_that_changes_nothing_is_not_news)
{
	MouseDevice mouse{};
	mouse.pushMotion(3, 0, MouseDevice::ButtonLeft, 0);
	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::StatusRegister), u32{ MouseDevice::StatusDataReady });

	// The same button mask again, no motion, no wheel: a program waiting for news would only spin on it.
	mouse.pushMotion(0, 0, MouseDevice::ButtonLeft, 0);
	CHECK_EQ(mouse.readUnsignedWord(MouseDevice::StatusRegister), u32{ 0 });
}

// --- The pixel display -------------------------------------------------------------------------

TEST(devices, the_display_shows_the_pixels_it_was_given)
{
	CeresVM vm{};
	DisplayDevice display{};
	display.attachTo(vm.io());

	display.writeWord(DisplayDevice::WidthRegister, 2);
	display.writeWord(DisplayDevice::HeightRegister, 1);

	// Two pixels in RAM, 0x00RRGGBB: red then green.
	vm.memory().writeUnchecked<u32>(Address(SourceBuffer), 0x00FF0000u);
	vm.memory().writeUnchecked<u32>(Address(SourceBuffer + 4), 0x0000FF00u);

	display.writeWord(DisplayDevice::BlockAddressRegister, SourceBuffer);
	display.writeWord(DisplayDevice::BlockLengthRegister, 8); // two pixels
	display.writeWord(DisplayDevice::BlockCommandRegister, DisplayDevice::BlockCommandWrite);

	u32 shownWidth = 0, shownHeight = 0;
	std::vector<u32> shown;
	display.setFrameSink([&](u32 width, u32 height, std::span<const u32> pixels)
	{
		shownWidth = width;
		shownHeight = height;
		shown.assign(pixels.begin(), pixels.end());
	});

	display.writeWord(DisplayDevice::CommandRegister, DisplayDevice::CommandPresent);

	CHECK_EQ(shownWidth, u32{ 2 });
	CHECK_EQ(shownHeight, u32{ 1 });
	CHECK_EQ(shown.size(), usize{ 2 });
	if (shown.size() == 2)
	{
		CHECK_EQ(shown[0], u32{ 0x00FF0000u });
		CHECK_EQ(shown[1], u32{ 0x0000FF00u });
	}
}

TEST(devices, a_display_pixel_can_be_written_one_at_a_time)
{
	DisplayDevice display{};

	display.writeWord(DisplayDevice::WidthRegister, 3);
	display.writeWord(DisplayDevice::HeightRegister, 1);

	display.writeWord(DisplayDevice::DataRegister, 0x00112233u);
	display.writeWord(DisplayDevice::DataRegister, 0x00445566u);

	CHECK_EQ(display.pixels()[0], u32{ 0x00112233u });
	CHECK_EQ(display.pixels()[1], u32{ 0x00445566u });
	CHECK_EQ(display.pixels()[2], u32{ 0 }); // The third cell was never written.
}

TEST(devices, a_display_surface_larger_than_any_screen_is_a_typo_and_is_ignored)
{
	DisplayDevice display{};

	const u32 before = display.width();
	display.writeWord(DisplayDevice::WidthRegister, 100000);
	CHECK_EQ(display.width(), before);

	display.writeWord(DisplayDevice::HeightRegister, 0);
	CHECK_EQ(display.height(), u32{ 200 });
}

TEST(devices, a_display_clear_fills_black)
{
	DisplayDevice display{};

	display.writeWord(DisplayDevice::WidthRegister, 2);
	display.writeWord(DisplayDevice::HeightRegister, 1);
	display.writeWord(DisplayDevice::DataRegister, 0x00FFFFFFu); // white

	display.writeWord(DisplayDevice::CommandRegister, DisplayDevice::CommandClear);

	CHECK_EQ(display.pixels()[0], u32{ 0 });
}

TEST(devices, an_indexed_display_goes_through_its_palette_and_scrolls)
{
	CeresVM vm{ Memory::DefaultSize };
	DisplayDevice display{};
	display.attachTo(vm.io());
	display.writeWord(DisplayDevice::WidthRegister, 3);
	display.writeWord(DisplayDevice::HeightRegister, 2);
	display.writeWord(DisplayDevice::ModeRegister, DisplayDevice::ModeIndexed);
	CHECK_EQ(display.readUnsignedWord(DisplayDevice::ModeRegister), DisplayDevice::ModeIndexed);
	display.writeWord(DisplayDevice::PaletteIndexRegister, 1);
	display.writeWord(DisplayDevice::PaletteDataRegister, 0x00FF0000u);   // 1: red
	display.writeWord(DisplayDevice::PaletteDataRegister, 0x0000FF00u);   // 2: green
	const u8 indices[6] = { 1, 2, 0, 0, 0, 2 };
	for (u32 i = 0; i < 6; ++i)
		vm.memory().writeUnchecked<u8>(Address(0x2000 + i), indices[i]);
	display.writeWord(DisplayDevice::BlockAddressRegister, 0x2000);
	display.writeWord(DisplayDevice::BlockLengthRegister, 6);            // a byte a pixel
	display.writeWord(DisplayDevice::BlockCommandRegister, DisplayDevice::BlockCommandWrite);
	std::vector<u32> shown;
	display.setFrameSink([&](u32, u32, std::span<const u32> pixels) { shown.assign(pixels.begin(), pixels.end()); });
	display.writeWord(DisplayDevice::CommandRegister, DisplayDevice::CommandPresent);
	CHECK(shown.size() == 6 && shown[0] == 0x00FF0000u && shown[1] == 0x0000FF00u && shown[2] == 0u && shown[5] == 0x0000FF00u);
	display.writeWord(DisplayDevice::ScrollXRegister, 1);                // the second column shows at the left
	display.writeWord(DisplayDevice::ScrollYRegister, 1);                // and the second row at the top
	display.writeWord(DisplayDevice::CommandRegister, DisplayDevice::CommandPresent);
	CHECK(shown.size() == 6 && shown[0] == 0u && shown[1] == 0x0000FF00u && shown[2] == 0u);   // row 1, from column 1, wrapping
	CHECK(shown.size() == 6 && shown[3] == 0x0000FF00u && shown[4] == 0u && shown[5] == 0x00FF0000u);
	CHECK(display.frame()[3] == 0x0000FF00u);                             // what a window draws
}

TEST(devices, the_blitter_fills_copies_keys_scales_and_indexes)
{
	CeresVM vm{ Memory::DefaultSize };
	BlitterDevice blitter{};
	blitter.attachTo(vm.io());
	auto px = [&](u32 address) { return vm.memory().readUnchecked<u32>(Address(address)); };
	using B = BlitterDevice;
	// A 4x3 destination surface at 0x10000 (stride 16), filled.
	blitter.writeWord(B::DstAddressRegister, 0x10000);
	blitter.writeWord(B::DstStrideRegister, 16);
	blitter.writeWord(B::WidthRegister, 4);
	blitter.writeWord(B::HeightRegister, 3);
	blitter.writeWord(B::ColorRegister, 0x00123456u);
	blitter.writeWord(B::CommandRegister, B::CommandFill);
	CHECK_EQ(blitter.readUnsignedWord(B::PixelsRegister), 12u);
	CHECK(px(0x10000) == 0x00123456u && px(0x10000 + 2 * 16 + 12) == 0x00123456u);
	// A 2x1 sprite with a transparent pixel, copied keyed at (1, 1).
	vm.memory().writeUnchecked<u32>(Address(0x20000), 0x00FF00FFu);          // the key
	vm.memory().writeUnchecked<u32>(Address(0x20004), 0x00ABCDEFu);
	blitter.writeWord(B::SrcAddressRegister, 0x20000);
	blitter.writeWord(B::SrcStrideRegister, 8);
	blitter.writeWord(B::DstAddressRegister, 0x10000 + 16 + 4);
	blitter.writeWord(B::WidthRegister, 2);
	blitter.writeWord(B::HeightRegister, 1);
	blitter.writeWord(B::ColorRegister, 0x00FF00FFu);
	blitter.writeWord(B::CommandRegister, B::CommandCopyKeyed);
	CHECK_EQ(blitter.readUnsignedWord(B::PixelsRegister), 1u);
	CHECK(px(0x10000 + 16 + 4) == 0x00123456u && px(0x10000 + 16 + 8) == 0x00ABCDEFu);
	// Scaled x2: one source pixel becomes a 2x2 block.
	blitter.writeWord(B::SrcAddressRegister, 0x20004);
	blitter.writeWord(B::DstAddressRegister, 0x30000);
	blitter.writeWord(B::DstStrideRegister, 8);
	blitter.writeWord(B::WidthRegister, 1);
	blitter.writeWord(B::HeightRegister, 1);
	blitter.writeWord(B::ScaleRegister, 2);
	blitter.writeWord(B::CommandRegister, B::CommandCopyScaled);
	CHECK_EQ(blitter.readUnsignedWord(B::PixelsRegister), 4u);
	CHECK(px(0x30000) == 0x00ABCDEFu && px(0x30004) == 0x00ABCDEFu && px(0x30008) == 0x00ABCDEFu && px(0x3000C) == 0x00ABCDEFu);
	// Indexed through a palette, index 0 left out.
	vm.memory().writeUnchecked<u32>(Address(0x40000 + 3 * 4), 0x00777777u);  // palette[3]
	vm.memory().writeUnchecked<u8>(Address(0x50000), 3);
	vm.memory().writeUnchecked<u8>(Address(0x50001), 0);
	blitter.writeWord(B::PaletteAddressRegister, 0x40000);
	blitter.writeWord(B::SrcAddressRegister, 0x50000);
	blitter.writeWord(B::SrcStrideRegister, 2);
	blitter.writeWord(B::DstAddressRegister, 0x10000);
	blitter.writeWord(B::DstStrideRegister, 16);
	blitter.writeWord(B::WidthRegister, 2);
	blitter.writeWord(B::ColorRegister, 0);
	blitter.writeWord(B::CommandRegister, B::CommandCopyIndexedKeyed);
	CHECK(px(0x10000) == 0x00777777u && px(0x10004) == 0x00123456u);
	// An overlapping copy one row down: rows go bottom-up, so every row arrives intact.
	blitter.writeWord(B::SrcAddressRegister, 0x10000);
	blitter.writeWord(B::SrcStrideRegister, 16);
	blitter.writeWord(B::DstAddressRegister, 0x10010);
	blitter.writeWord(B::WidthRegister, 4);
	blitter.writeWord(B::HeightRegister, 2);
	blitter.writeWord(B::CommandRegister, B::CommandCopy);
	CHECK(px(0x10010) == 0x00777777u && px(0x10020 + 8) == 0x00ABCDEFu);
	// Outside RAM: the error bit, and the interrupt when asked for.
	blitter.writeWord(B::ControlRegister, B::ControlInterrupt);
	blitter.writeWord(B::DstAddressRegister, static_cast<u32>(vm.memory().size()) - 8);
	blitter.writeWord(B::CommandRegister, B::CommandFill);
	CHECK_EQ(blitter.readUnsignedWord(B::StatusRegister), B::StatusError);
	CHECK((vm.interrupts().pendingMask() & (u64{ 1 } << static_cast<u8>(B::Interrupt))) != 0);
}

// --- The gamepad -------------------------------------------------------------------------------

TEST(devices, the_gamepad_reports_buttons_and_axes)
{
	GamepadDevice gamepad{};

	gamepad.pushState(GamepadDevice::ButtonSouth | GamepadDevice::ButtonDpadRight, -100, 200, 0, -32768, 32767, 0);

	CHECK_EQ(gamepad.readUnsignedWord(GamepadDevice::ButtonsRegister),
		u32{ GamepadDevice::ButtonSouth | GamepadDevice::ButtonDpadRight });
	CHECK_EQ(gamepad.readUnsignedWord(GamepadDevice::LeftXRegister), static_cast<u32>(-100));
	CHECK_EQ(gamepad.readUnsignedWord(GamepadDevice::LeftYRegister), u32{ 200 });
	CHECK_EQ(gamepad.readUnsignedWord(GamepadDevice::RightYRegister), static_cast<u32>(-32768));
	CHECK_EQ(gamepad.readUnsignedWord(GamepadDevice::LeftTriggerRegister), u32{ 32767 });
	CHECK_EQ(gamepad.readUnsignedWord(GamepadDevice::RightTriggerRegister), u32{ 0 });
}

TEST(devices, a_gamepad_state_change_is_reported_until_read)
{
	GamepadDevice gamepad{};

	CHECK_EQ(gamepad.readUnsignedWord(GamepadDevice::StatusRegister), u32{ 0 });

	// The resting state is not a change.
	gamepad.pushState(0, 0, 0, 0, 0, 0, 0);
	CHECK_EQ(gamepad.readUnsignedWord(GamepadDevice::StatusRegister), u32{ 0 });

	gamepad.pushState(GamepadDevice::ButtonSouth, 0, 0, 0, 0, 0, 0);
	CHECK_EQ(gamepad.readUnsignedWord(GamepadDevice::StatusRegister) & GamepadDevice::StatusChanged, GamepadDevice::StatusChanged);

	// Reading the status consumes the change.
	CHECK_EQ(gamepad.readUnsignedWord(GamepadDevice::StatusRegister), u32{ 0 });
}

TEST(devices, a_gamepad_state_change_raises_the_gamepads_interrupt)
{
	Machine m{ Instruction::STI(), Instruction::NOP(), Instruction::NOP() };

	GamepadDevice gamepad{};
	gamepad.attachTo(m.vm().io());

	m.installHandler(GamepadDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x71),
		Instruction::IRET(),
	});

	gamepad.pushState(GamepadDevice::ButtonSouth, 0, 0, 0, 0, 0, 0);
	m.step(3);

	CHECK_EQ(m.reg(9), 0x71u);
}

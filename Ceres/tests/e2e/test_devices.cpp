// The timer and alignment faults: the two things the machine could not do before.
//
// Devices used to be entirely passive, so nothing could wake a halted machine, and nothing
// depended on alignment so AlignmentFault was never raised.

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

TEST(devices, a_program_can_arm_the_timer_through_its_port)
{
	Machine m{
		Instruction::STI(),
		Instruction::LI(1, 3),
		Instruction::OUT(1, TimerDevice::CommandPort),
		Instruction::NOP(), Instruction::NOP(), Instruction::NOP(), Instruction::NOP(),
	};

	TimerDevice timer{};
	timer.attachTo(m.vm().io());

	m.installHandler(TimerDevice::Interrupt, Address(0x800), {
		Instruction::LI(9, 0x33),
		Instruction::IRET(),
	});

	m.step(10);

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

// --- The terminal's output sink ----------------------------------------------------------------

TEST(devices, terminal_output_goes_to_the_installed_sink_instead_of_stdout)
{
	// OUTB writes one byte; OUT writes a whole word, low byte first. Together they cover both
	// paths through emitByte.
	Machine m{
		Instruction::LI(1, 'C'),
		Instruction::OUTB(1, TerminalDevice::OutputPort),
		Instruction::LI(2, 0x0000'6165), // 'e', 'a', 0, 0
		Instruction::OUT(2, TerminalDevice::OutputPort),
	};

	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());

	std::string captured;
	terminal.setOutputSink([&captured](u8 byte) { captured.push_back(static_cast<char>(byte)); });

	m.step(4);

	CHECK_EQ(captured.size(), usize{ 5 });
	CHECK(captured.starts_with("Cea"));
}

TEST(devices, a_multi_byte_character_reaches_the_sink_one_byte_at_a_time)
{
	// 'á' is 0xC3 0xA1 in UTF-8. The device hands over bytes, not characters: a consumer that
	// decoded each one on its own would produce two replacement characters instead of one letter.
	Machine m{
		Instruction::LI(1, 0xC3),
		Instruction::OUTB(1, TerminalDevice::OutputPort),
		Instruction::LI(1, 0xA1),
		Instruction::OUTB(1, TerminalDevice::OutputPort),
	};

	TerminalDevice terminal{};
	terminal.attachTo(m.vm().io());

	std::vector<u8> captured;
	terminal.setOutputSink([&captured](u8 byte) { captured.push_back(byte); });

	m.step(4);

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
// Ports 0x20-0x23 and 0x30-0x33 were named in the port map from the start and answered nothing.
// A program could read them and get 0xFF back, which is what an absent device looks like.

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
		Instruction::LI(1, 1),
		Instruction::OUT(1, DiskDevice::SectorPort),
		Instruction::LI(2, static_cast<u16>(SourceBuffer)),
		Instruction::LI(3, 8),
		Instruction::OUTM(2, 3, DiskDevice::DataPort),   // memory -> sector 1
		Instruction::LI(4, static_cast<u16>(DestinationBuffer)),
		Instruction::INM(4, 3, DiskDevice::DataPort),    // sector 1 -> memory, somewhere else
	};

	DiskDevice disk{};
	disk.attachTo(m.vm().io());

	fill(m.memory(), SourceBuffer, "ON A DISK");
	m.step(7);

	CHECK_EQ(readBack(m.memory(), DestinationBuffer, 8), std::string{ "ON A DIS" });

	// And the sector it did not select is untouched, which is the whole reason for selecting one.
	CHECK_EQ(disk.image()[0], u8{ 0 });
}

TEST(devices, a_sector_past_the_end_of_the_disk_is_refused_rather_than_wrapped)
{
	Machine m{
		Instruction::LI(1, 9999),
		Instruction::OUT(1, DiskDevice::SectorPort),
		Instruction::LI(2, static_cast<u16>(SourceBuffer)),
		Instruction::LI(3, 8),
		Instruction::OUTM(2, 3, DiskDevice::DataPort),
		Instruction::INB(5, DiskDevice::StatusPort),
	};

	DiskDevice disk{ 4 }; // Four sectors, so 9999 is nowhere
	disk.attachTo(m.vm().io());

	fill(m.memory(), SourceBuffer, "NOWHERE");
	m.step(6);

	CHECK_EQ(m.reg(5) & DiskDevice::StatusError, DiskDevice::StatusError);
	CHECK_EQ(m.reg(5) & DiskDevice::StatusReady, 0u);
}

TEST(devices, a_file_backed_disk_still_holds_what_was_written_after_the_machine_stops)
{
	const auto path = std::filesystem::temp_directory_path() / "ceres_test_disk.img";
	std::filesystem::remove(path);

	{
		Machine m{
			Instruction::LI(1, 2),
			Instruction::OUT(1, DiskDevice::SectorPort),
			Instruction::LI(2, static_cast<u16>(SourceBuffer)),
			Instruction::LI(3, 6),
			Instruction::OUTM(2, 3, DiskDevice::DataPort),
			Instruction::LI(4, DiskDevice::CommandFlush),
			Instruction::OUT(4, DiskDevice::CommandPort),
		};

		DiskDevice disk{};
		CHECK(disk.open(path, 8));
		disk.attachTo(m.vm().io());

		fill(m.memory(), SourceBuffer, "PERSIST");
		m.step(7);
	}

	// A second machine, a second device, the same file.
	Machine m{
		Instruction::LI(1, 2),
		Instruction::OUT(1, DiskDevice::SectorPort),
		Instruction::LI(2, static_cast<u16>(DestinationBuffer)),
		Instruction::LI(3, 6),
		Instruction::INM(2, 3, DiskDevice::DataPort),
	};

	DiskDevice disk{};
	CHECK(disk.open(path, 8));
	disk.attachTo(m.vm().io());
	m.step(5);

	CHECK_EQ(readBack(m.memory(), DestinationBuffer, 6), std::string{ "PERSIS" });
	std::filesystem::remove(path);
}

TEST(devices, the_framebuffer_shows_the_grid_it_was_given)
{
	Machine m{
		Instruction::LI(1, 4),
		Instruction::OUT(1, FramebufferDevice::WidthPort),
		Instruction::LI(1, 2),
		Instruction::OUT(1, FramebufferDevice::HeightPort),
		Instruction::LI(1, FramebufferDevice::CommandClear),
		Instruction::OUT(1, FramebufferDevice::CommandPort),
		Instruction::LI(2, static_cast<u16>(SourceBuffer)),
		Instruction::LI(3, 8),
		Instruction::OUTM(2, 3, FramebufferDevice::DataPort),
		Instruction::LI(1, FramebufferDevice::CommandPresent),
		Instruction::OUT(1, FramebufferDevice::CommandPort),
	};

	FramebufferDevice framebuffer{};
	framebuffer.attachTo(m.vm().io());

	std::string shown;
	framebuffer.setPresentSink([&shown](std::string_view frame) { shown = frame; });

	fill(m.memory(), SourceBuffer, "ab..#..#");
	m.step(11);

	// Rows, not a stream: the second four cells are the second line.
	CHECK_EQ(shown, std::string{ "ab..\n#..#\n" });
}

TEST(devices, a_cell_that_would_move_the_terminals_own_cursor_is_shown_as_a_space)
{
	Machine m{ Instruction::NOP() };

	FramebufferDevice framebuffer{};
	framebuffer.attachTo(m.vm().io());

	framebuffer.writePortWord(FramebufferDevice::WidthPort, 3);
	framebuffer.writePortWord(FramebufferDevice::HeightPort, 1);
	framebuffer.writePortWord(FramebufferDevice::DataPort, 'x');
	framebuffer.writePortWord(FramebufferDevice::DataPort, 0x07); // a bell
	framebuffer.writePortWord(FramebufferDevice::DataPort, 'y');

	CHECK_EQ(framebuffer.toText(), std::string{ "x y\n" });
}

TEST(devices, a_grid_larger_than_any_terminal_is_a_typo_and_is_ignored)
{
	Machine m{ Instruction::NOP() };

	FramebufferDevice framebuffer{};
	framebuffer.attachTo(m.vm().io());

	const u32 before = framebuffer.width();
	framebuffer.writePortWord(FramebufferDevice::WidthPort, 100000);
	CHECK_EQ(framebuffer.width(), before);

	framebuffer.writePortWord(FramebufferDevice::HeightPort, 0);
	CHECK_EQ(framebuffer.height(), u32{ 20 });
}

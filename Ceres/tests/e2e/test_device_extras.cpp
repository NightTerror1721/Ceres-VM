// What the standard library asked of the machine: the end-of-input flag, an exit status, an
// STI that takes effect a step late, the capacities a program could not learn, a millisecond
// clock, colour in the text grid, an optional fault on division by zero, typed characters and a
// tone generator. Every one of them is opt-in or an addition: nothing here changes what a program
// written before them does.

#include "framework.h"
#include <ceres/vm/ceresvm.h>
#include <ceres/devices/devices.h>
#include <ceres/devices/storage/disk.h>
#include <ceres/devices/video/text_framebuffer.h>
#include <ceres/devices/input/keyboard.h>
#include <ceres/devices/audio/audio.h>
#include <ceres/vm/bios.h>
#include <ceres/core/format/memory_map.h>
#include <filesystem>
#include <fstream>
#include <optional>
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
		Memory& memory() { return _vm.memory(); }

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

	constexpr u8 Base = 13;
	constexpr u16 Hi(Address address) noexcept { return static_cast<u16>(address.value() >> 16); }
	constexpr u16 Lo(Address address) noexcept { return static_cast<u16>(address.value() & 0xFFFF); }
	Instruction LoadBase(Address address) noexcept { return Instruction::LUI(Base, Hi(address)); }
	Instruction LoadBaseLow(Address address) noexcept { return Instruction::ORI(Base, Base, Lo(address)); }
	u16 Off(Address registerOffset) noexcept { return static_cast<u16>(registerOffset.value()); }

	constexpr u32 SourceBuffer = 0x1000;

	void fill(Memory& memory, u32 address, std::string_view bytes)
	{
		for (u32 i = 0; i < bytes.size(); ++i)
			memory.writeUnchecked<u8>(Address(address + i), static_cast<u8>(bytes[i]));
	}
}

// --- Division by zero, as an option ------------------------------------------------------------

TEST(device_extras, division_by_zero_only_sets_the_trap_flag_unless_asked_for_more)
{
	Machine m{
		Instruction::LI(1, 7),
		Instruction::LI(2, 0),
		Instruction::LI(3, 99),
		Instruction::DIV(3, 1, 2),
		Instruction::LI(4, 4),
	};

	m.installHandler(InterruptNumber::DivisionByZero, Address(0x800), {
		Instruction::LI(8, 0xD),
		Instruction::IRET(),
	});

	m.step(6);

	CHECK(m.flags().trap());
	CHECK_EQ(m.reg(3), 99u);
	CHECK_EQ(m.reg(8), 0u);
	CHECK_EQ(m.reg(4), 4u);
}

TEST(device_extras, division_by_zero_can_raise_its_own_interrupt)
{
	Machine m{
		Instruction::LI(1, 7),
		Instruction::LI(2, 0),
		Instruction::LI(3, 99),
		Instruction::DIV(3, 1, 2),
		Instruction::LI(4, 4),
	};

	m.vm().engine().setDivisionFaults(true);
	m.installHandler(InterruptNumber::DivisionByZero, Address(0x800), {
		Instruction::LI(8, 0xD),
		Instruction::IRET(),
	});

	m.step(8);

	CHECK_EQ(m.reg(8), 0xDu);  // The handler ran
	CHECK_EQ(m.reg(3), 99u);   // The destination was left alone
	CHECK_EQ(m.reg(4), 4u);    // And it came back to the instruction after the division
	CHECK(!m.flags().trap());
}

TEST(device_extras, an_ieee_float_division_by_zero_gives_infinity_or_nan)
{
	// With FeatureIeeeDivide the float divisions answer as IEEE 754 does, and nothing traps; DIV still does.
	Machine m{
		Instruction::LI(1, 7),
		Instruction::LI(2, 0),
		Instruction::ITOF(0, 1),                // 7
		Instruction::ITOF(1, 2),                // +0
		Instruction::FNEG(2, 1),                // -0
		Instruction::FDIV(3, 0, 1),             // 7 / +0
		Instruction::FDIV(4, 0, 2),             // 7 / -0
		Instruction::FDIV(5, 1, 1),             // 0 / 0
		Instruction::FMOD(6, 0, 1),             // fmod(7, 0)
		Instruction::FRECIPE(7, 2),             // 1 / -0
		Instruction::FRSQRTE(8, 1),             // 1 / sqrt(+0)
		Instruction::FCLASS(3, 3),
		Instruction::FCLASS(4, 4),
		Instruction::FCLASS(5, 5),
		Instruction::FCLASS(6, 6),
		Instruction::FCLASS(7, 7),
		Instruction::FCLASS(8, 8),
		Instruction::DIV(9, 1, 2),              // an integer division is not changed
	};
	m.vm().engine().setIeeeDivide(true);
	m.step(17);
	CHECK(!m.flags().trap());      // no float division trapped
	m.step(1);

	CHECK_EQ(m.reg(3), 1u << 7);   // +inf
	CHECK_EQ(m.reg(4), 1u << 0);   // -inf
	CHECK_EQ(m.reg(5), 1u << 8);   // NaN
	CHECK_EQ(m.reg(6), 1u << 8);   // NaN
	CHECK_EQ(m.reg(7), 1u << 0);   // -inf
	CHECK_EQ(m.reg(8), 1u << 7);   // +inf
	CHECK(m.flags().trap());       // the DIV at the end still does
}

TEST(device_extras, every_division_shaped_instruction_honours_the_option)
{
	// A remainder and a float division take the same road as DIV.
	Machine m{
		Instruction::LI(1, 7),
		Instruction::LI(2, 0),
		Instruction::MOD(3, 1, 2),
		Instruction::ITOF(0, 1),
		Instruction::ITOF(1, 2),
		Instruction::FDIV(2, 0, 1),
		Instruction::LI(4, 4),
	};

	m.vm().engine().setDivisionFaults(true);
	m.installHandler(InterruptNumber::DivisionByZero, Address(0x800), {
		Instruction::ADDI(8, 8, 1),
		Instruction::IRET(),
	});

	m.step(12);

	CHECK_EQ(m.reg(8), 2u);
	CHECK_EQ(m.reg(4), 4u);
}

TEST(device_extras, a_block_instruction_costs_the_clock_its_length_in_cycles)
{
	// A page copied in one step: 4 cycles for the instruction and one for every 8 bytes (plan/v2 SPEC 3.2).
	Machine m{ Instruction::MCPY(1, 2, 3) };
	TimerDevice timer{};
	timer.attachTo(m.vm().io());
	m.vm().engine().setRegister(1, 0x20000);
	m.vm().engine().setRegister(2, 0x10000);
	m.vm().engine().setRegister(3, 4096);
	const u64 before = timer.cycles();
	m.step();
	CHECK_EQ(timer.cycles() - before, u64{ isa::cycles::BlockBase + 4096 / 8 });
	timer.detachFrom(m.vm().io());
}

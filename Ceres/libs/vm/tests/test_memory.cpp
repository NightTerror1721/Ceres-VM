// The machine's RAM: host pages reserved lazily, and every size up to 2 GiB (plan/v2 F4.1).

#include "framework.h"
#include <ceres/core/base/host_pages.h>
#include <ceres/vm/ceresvm.h>
#include <stdexcept>
#include <utility>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::testing;

TEST(memory, host_pages_read_as_zero_and_move)
{
	HostPages pages{ 1024 * 1024 };
	CHECK_EQ(pages.size(), usize{ 1024 * 1024 });
	CHECK_EQ(pages[0], u8{ 0 });
	CHECK_EQ(pages[pages.size() - 1], u8{ 0 });

	pages[4096] = 0x5A;
	HostPages moved{ std::move(pages) };
	CHECK(pages.data() == nullptr);
	CHECK_EQ(pages.size(), usize{ 0 });
	CHECK_EQ(moved[4096], u8{ 0x5A });

	HostPages assigned;
	assigned = std::move(moved);
	CHECK_EQ(assigned[4096], u8{ 0x5A });
	CHECK(moved.data() == nullptr);
}

TEST(memory, sizes_run_from_8_kib_to_2_gib)
{
	bool refusedSmall = false;
	try { Memory small{ Memory::MinSize - 1 }; }
	catch (const std::invalid_argument&) { refusedSmall = true; }
	CHECK(refusedSmall);

	bool refusedLarge = false;
	try { Memory large{ Memory::MaxSize + 1 }; }
	catch (const std::invalid_argument&) { refusedLarge = true; }
	CHECK(refusedLarge);

	CHECK_EQ(Memory::MaxSize, usize{ 0x80000000 });
}

TEST(memory, a_2_gib_machine_boots_and_runs_a_program)
{
	// All of 0x00000000-0x7FFFFFFF: the stacks start at the very top, where RamSize is 0x80000000 and no longer
	// fits in 31 bits. The program stores into the middle of the RAM, pushes and pops, and takes an interrupt,
	// whose handler runs on the system stack from 0x80000000 down.
	CeresVM vm{ Memory::MaxSize };
	CHECK_EQ(vm.memory().size(), usize{ 0x80000000 });

	const Address entry = Memory::UnrestrictedSegmentStart;
	const Address handler = Address(0x1000);
	const Instruction program[] = {
		Instruction::LUI(1, 0x4000),        // r1 = 0x40000000
		Instruction::LI(2, 1234),
		Instruction::STR(1, 2, 0),
		Instruction::LDR(3, 1, 0),
		Instruction::PUSH(3),
		Instruction::POP(4),
		Instruction::STI(),
		Instruction::INT(48),
	};
	u32 offset = 0;
	for (const Instruction instruction : program)
	{
		vm.memory().writeUnchecked<u32>(entry + Address(offset), instruction.raw());
		offset += Instruction::Size;
	}
	vm.memory().writeUnchecked<u32>(handler, Instruction::IRET().raw());
	vm.memory().writeUnchecked<u32>(Address(48 * Address::Size), handler.value());
	vm.memory().writeUnchecked<u32>(0_addr, entry.value());
	vm.engine().reset();

	CHECK_EQ(vm.engine().registers().getValue(15), 0x80000000u - static_cast<u32>(Memory::SystemStackSize));

	for (usize i = 0; i < 7; ++i)
		vm.engine().step();
	CHECK_EQ(vm.memory().read<u32>(Address(0x40000000)), 1234u);
	CHECK_EQ(vm.engine().registers().getValue(3), 1234u);
	CHECK_EQ(vm.engine().registers().getValue(4), 1234u);

	vm.engine().step(); // int 48
	CHECK_EQ(vm.engine().programCounter().value(), handler.value());
	CHECK_EQ(vm.engine().registers().getValue(15), 0x80000000u - 2 * Address::Size);

	vm.engine().step(); // iret
	CHECK_EQ(vm.engine().programCounter().value(), entry.value() + offset);
	CHECK_EQ(vm.engine().registers().getValue(15), 0x80000000u - static_cast<u32>(Memory::SystemStackSize));
}

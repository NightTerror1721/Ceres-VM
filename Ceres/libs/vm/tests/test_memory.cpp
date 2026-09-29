// The machine's RAM and VRAM: host pages reserved lazily, every RAM size up to 2 GiB (plan/v2 F4.1), and the
// physical map that routes an access to the RAM, the VRAM, the devices or a fault (F4.2, SPEC 2).

#include "framework.h"
#include <ceres/core/base/host_pages.h>
#include <ceres/vm/ceresvm.h>
#include <stdexcept>
#include <utility>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::testing;

namespace
{
	using FaultAccess = ExecutionEngine::FaultAccess;

	constexpr Address FaultHandler = Address(0x3000);

	// A program at 0x400 with a MemoryFault handler (a halt) at 0x3000, stepped by hand.
	class Machine
	{
	private:
		CeresVM _vm;

	public:
		explicit Machine(std::initializer_list<Instruction> program, usize ramSize = Memory::DefaultSize, usize vramSize = Vram::DefaultSize) :
			_vm(ramSize, vramSize)
		{
			const Address entry = Memory::UnrestrictedSegmentStart;
			u32 offset = 0;
			for (const Instruction instruction : program)
			{
				_vm.memory().writeUnchecked<u32>(entry + Address(offset), instruction.raw());
				offset += Instruction::Size;
			}
			_vm.memory().writeUnchecked<u32>(FaultHandler, Instruction::HALT().raw());
			_vm.memory().writeUnchecked<u32>(Address(static_cast<u32>(InterruptNumber::MemoryFault) * Address::Size), FaultHandler.value());
			_vm.memory().writeUnchecked<u32>(0_addr, entry.value());
			_vm.engine().reset();
		}

		void step(usize count = 1)
		{
			for (usize i = 0; i < count; ++i)
				_vm.engine().step();
		}

		CeresVM& vm() noexcept { return _vm; }
		ExecutionEngine& engine() noexcept { return _vm.engine(); }
		u32 reg(u8 index) const { return _vm.engine().registers().getValue(index); }
		void set(u8 index, u32 value) { _vm.engine().setRegister(index, value); }
		u32 pc() const { return _vm.engine().programCounter().value(); }
	};

	u32 accessWord(FaultAccess access, u32 size) { return static_cast<u32>(access) | (size << 8); }

	// Runs one access at `address` (in r1) and says whether it faulted with `reason`, at that address.
	bool faultsWith(Instruction access, u32 address, FaultReason reason, FaultAccess kind, u32 size)
	{
		Machine m{ access };
		m.set(1, address);
		m.step();
		return m.pc() == FaultHandler.value() && m.engine().faultReason() == static_cast<u32>(reason) &&
			m.engine().faultAddress() == address && m.engine().faultAccess() == accessWord(kind, size);
	}
}

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

	bool refusedOdd = false;
	try { Memory odd{ Memory::MinSize + 4 }; }
	catch (const std::invalid_argument&) { refusedOdd = true; }
	CHECK(refusedOdd);

	bool refusedVram = false;
	try { Vram small{ Vram::MinSize - Vram::PageSize }; }
	catch (const std::invalid_argument&) { refusedVram = true; }
	CHECK(refusedVram);
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

TEST(memory, each_region_of_the_physical_map_answers_as_the_spec_says)
{
	// RAM and VRAM take a load and a store; past either one, and in the two empty regions, the access is a
	// MemoryFault that says which (plan/v2 SPEC 2 and 5.4).
	constexpr usize Ram = 1024 * 1024;
	constexpr usize VramBytes = 64 * 1024;
	{
		Machine m({ Instruction::LI(2, 0x5A5A), Instruction::STR(1, 2, 0), Instruction::LDR(3, 1, 0) }, Ram, VramBytes);
		m.set(1, Vram::BaseValue + 0x100);
		m.step(3);
		CHECK_EQ(m.reg(3), 0x5A5Au);
		CHECK_EQ(m.vm().vram().read<u32>(0x100), 0x5A5Au);
	}

	const Instruction load = Instruction::LDR(3, 1, 0);
	const Instruction store = Instruction::STRB(1, 2, 0);
	CHECK(faultsWith(load, static_cast<u32>(Memory::DefaultSize), FaultReason::OutOfRam, FaultAccess::Read, 4));
	CHECK(faultsWith(store, 0x7FFFFFFF, FaultReason::OutOfRam, FaultAccess::Write, 1));
	CHECK(faultsWith(load, 0x80000000, FaultReason::Unmapped, FaultAccess::Read, 4));
	CHECK(faultsWith(store, 0x9FFFFFFF, FaultReason::Unmapped, FaultAccess::Write, 1));
	CHECK(faultsWith(load, Vram::BaseValue + static_cast<u32>(Vram::DefaultSize), FaultReason::OutOfVram, FaultAccess::Read, 4));
	CHECK(faultsWith(store, 0xDFFFFFFF, FaultReason::OutOfVram, FaultAccess::Write, 1));
	CHECK(faultsWith(load, 0xE0000000, FaultReason::Unmapped, FaultAccess::Read, 4));
	CHECK(faultsWith(store, 0xFEFFFFFF, FaultReason::Unmapped, FaultAccess::Write, 1));

	// The last halfword and the last word of the RAM are in it.
	{
		Machine m({ Instruction::LDRH(3, 1, 0) }, Memory::MinSize);
		m.set(1, static_cast<u32>(Memory::MinSize) - 2);
		m.step();
		CHECK_EQ(m.pc(), 0x400u + 4u);   // the last halfword is in
		Machine n({ Instruction::LDR(3, 1, 0) }, Memory::MinSize);
		n.set(1, static_cast<u32>(Memory::MinSize) - 4);
		n.step();
		CHECK_EQ(n.pc(), 0x400u + 4u);
	}
}

TEST(memory, a_vram_access_costs_three_cycles_and_marks_its_page)
{
	// SPEC 3.2: 2 cycles for the RAM, 3 for the VRAM; a 64-bit access is one of either (SPEC 6.4).
	auto cyclesOf = [](Instruction instruction, u32 address)
	{
		Machine m{ instruction };
		m.set(1, address);
		m.step();
		return m.engine().cycles();
	};
	const u32 ram = 0x10000;
	const u32 vram = Vram::BaseValue + 0x10000;
	CHECK_EQ(cyclesOf(Instruction::LDR(3, 1, 0), vram), cyclesOf(Instruction::LDR(3, 1, 0), ram) + 1);
	CHECK_EQ(cyclesOf(Instruction::STR(1, 3, 0), vram), cyclesOf(Instruction::STR(1, 3, 0), ram) + 1);
	CHECK_EQ(cyclesOf(Instruction::LDRD(2, 1, 0), vram), cyclesOf(Instruction::LDRD(2, 1, 0), ram) + 1);

	Machine m{ Instruction::STRB(1, 2, 0), Instruction::STRD(1, 2, 0x1000), Instruction::LDR(3, 1, 0x2000) };
	m.set(1, Vram::BaseValue + 5 * Vram::PageSize);
	m.step(3);
	CHECK(m.vm().vram().written(5));
	CHECK(m.vm().vram().written(6));
	CHECK(!m.vm().vram().written(7));   // a load marks nothing
	CHECK_EQ(m.vm().vram().writtenPages(), usize{ 2 });
	m.vm().vram().clearWritten();
	CHECK_EQ(m.vm().vram().writtenPages(), usize{ 0 });
}

TEST(memory, a_write_gate_can_drop_the_cpus_stores_to_the_vram)
{
	// The gate of micro and pocket (plan/v2 SPEC 7.5): a store it refuses is not made and the CPU does not fault; a
	// block store it refuses runs to its end, writing nowhere.
	struct Gate final : VramWriteGate
	{
		bool open = false;
		u32 refused = 0;
		u32 last = 0;
		bool admitCpuStore(u32 physical) noexcept override
		{
			if (open)
				return true;
			++refused;
			last = physical;
			return false;
		}
	} gate;
	constexpr u32 At = Vram::BaseValue + 0x100;
	Machine m{ Instruction::LI(2, 0x5A5A), Instruction::STR(1, 2, 0), Instruction::MSET(4, 2, 3), Instruction::STR(1, 2, 0) };
	m.vm().vram().setWriteGate(&gate);
	m.set(1, At);
	m.set(3, 16);
	m.set(4, At + 0x100);
	m.step(2);
	CHECK_EQ(m.vm().vram().read<u32>(0x100), 0u);
	CHECK_EQ(gate.refused, 1u);
	CHECK_EQ(gate.last, At);
	CHECK_EQ(m.pc(), Memory::UnrestrictedSegmentStart.value() + 8);   // no fault
	m.step();
	CHECK_EQ(m.reg(3), 0u);
	CHECK_EQ(m.vm().vram().read<u32>(0x200), 0u);
	CHECK_EQ(gate.refused, 2u);
	gate.open = true;
	m.step();
	CHECK_EQ(m.vm().vram().read<u32>(0x100), 0x5A5Au);
	m.vm().vram().setWriteGate(nullptr);
}

TEST(memory, the_block_instructions_run_on_the_vram_and_fault_past_the_ram)
{
	// mset fills the VRAM in one span and marks the pages it covers; mcpy brings the bytes back to the RAM.
	Machine m{ Instruction::MSET(1, 2, 3), Instruction::MCPY(4, 5, 6) };
	m.set(1, Vram::BaseValue + Vram::PageSize - 16);
	m.set(2, 0xAB);
	m.set(3, 32);
	m.set(4, 0x20000);
	m.set(5, Vram::BaseValue + Vram::PageSize - 16);
	m.set(6, 32);
	m.step(4);   // each crosses a page: two chunks
	CHECK_EQ(m.reg(3), 0u);
	CHECK_EQ(m.reg(6), 0u);
	CHECK(m.vm().vram().written(0));
	CHECK(m.vm().vram().written(1));
	CHECK_EQ(m.vm().memory().read<u32>(Address(0x20000)), 0xABABABABu);
	CHECK_EQ(m.vm().memory().read<u32>(Address(0x2001C)), 0xABABABABu);

	// Past the end of the RAM, the chunk that has nothing behind it faults and leaves the registers where it
	// started, for the handler's iret to run it again.
	constexpr usize Ram = 64 * 1024;
	Machine n({ Instruction::MSET(1, 2, 3) }, Ram);
	n.set(1, static_cast<u32>(Ram) - 16);
	n.set(2, 0xCD);
	n.set(3, 64);
	n.step();
	CHECK_EQ(n.reg(3), 48u);
	n.step();
	CHECK_EQ(n.pc(), FaultHandler.value());
	CHECK_EQ(n.engine().faultReason(), static_cast<u32>(FaultReason::OutOfRam));
	CHECK_EQ(n.engine().faultAddress(), static_cast<u32>(Ram));
	CHECK_EQ(n.reg(1), static_cast<u32>(Ram));
	CHECK_EQ(n.reg(3), 48u);
}

TEST(memory, code_runs_from_the_vram_and_nowhere_past_it)
{
	Machine m{ Instruction::JPR(1) };
	m.vm().vram().write<u32>(0x40, Instruction::LI(5, 77).raw());
	m.vm().vram().write<u32>(0x44, Instruction::HALT().raw());
	m.set(1, Vram::BaseValue + 0x40);
	m.step(2);
	CHECK_EQ(m.reg(5), 77u);

	auto fetchFault = [](u32 target)
	{
		Machine f{ Instruction::JPR(1) };
		f.set(1, target);
		f.step(2);
		return std::pair{ f.engine().faultReason(), f.engine().faultAddress() };
	};
	CHECK_EQ(fetchFault(static_cast<u32>(Memory::DefaultSize)).first, static_cast<u32>(FaultReason::OutOfRam));
	CHECK_EQ(fetchFault(0x90000000).first, static_cast<u32>(FaultReason::Unmapped));
	CHECK_EQ(fetchFault(0xC0000000).first, static_cast<u32>(FaultReason::OutOfVram));
	CHECK_EQ(fetchFault(0xFF000000).first, static_cast<u32>(FaultReason::MmioWidth));
	CHECK_EQ(fetchFault(0x90000000).second, 0x90000000u);
}

// The MMU: two-level page tables, a small TLB, and the seven opcodes that drive them (MTP, MFP,
// PGON, PGOFF, INVLPG, FLPG, MFPF). Paging did not exist before this; every address was physical.
//
// See docs/27-Virtual-Memory-and-Paging.md for the encoding these tests exercise.

#include "framework.h"
#include <ceres/vm/ceresvm.h>
#include <ceres/vm/bios.h>
#include <ceres/core/format/memory_map.h>
#include <string_view>

using namespace ceres;
using namespace ceres::vm;
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

	// A page directory and two page tables, each one page (4 KiB) and - like every directory/table
	// entry's target - 4 KiB-aligned, or PteFrameMask would silently truncate it to whatever 4 KiB
	// page it actually falls in. Kept well clear of both the program text (which starts at
	// Memory::UnrestrictedSegmentStart, 0x400) and of the frames the tables point into.
	constexpr u32 PageDirectory = 0x2000;
	// Directory slot 0: identity-maps VA 0x000-0xFFF to the same physical page. Every test below that
	// turns paging on needs this - the instant PGON runs, the *next instruction fetch* is translated
	// too, and that next instruction is still sitting in this same first page (the program is a
	// couple of dozen words at most, and the BIOS's own stub lives below 0x400, which never needs
	// translation at all - see the "below UnrestrictedSegmentStart" exemption in ExecutionEngine).
	constexpr u32 CodeTable = 0x3000;
	// Directory slot 2: the "interesting" mapping most tests below actually examine.
	constexpr u32 DataTable = 0x4000;
	constexpr u32 FrameA = 0x5000;
	constexpr u32 FrameB = 0x6000;

	// Chosen so the split is exact and easy to hand-check: 0x00800000 >> 22 == 2 (directory index),
	// (0x00800000 >> 12) & 0x3FF == 0 (table index), and the low 12 bits are 0 (page-aligned).
	constexpr u32 MappedVA = 0x00800000;

	void mapPage(Machine& m, u32 directory, u32 table, u32 dirIndex, u32 tableIndex, u32 frame, u32 flags)
	{
		m.memory().writeUnchecked<u32>(Address(directory + dirIndex * 4), table | Mmu::PtePresent);
		m.memory().writeUnchecked<u32>(Address(table + tableIndex * 4), (frame & Mmu::PteFrameMask) | flags);
	}

	// Every test that calls PGON needs this: an identity map for the page the program (and, if it
	// installs one, its own PageFault handler) is actually running out of.
	void mapCodeIdentity(Machine& m)
	{
		mapPage(m, PageDirectory, CodeTable, /*dirIndex*/ 0, /*tableIndex*/ 0, /*frame*/ 0x000,
			Mmu::PtePresent | Mmu::PteWritable | Mmu::PteExecutable);
	}
}

// --- Translation ---------------------------------------------------------------------------------

TEST(paging, a_mapped_page_translates_reads_and_writes_to_its_physical_frame)
{
	Machine m{
		Instruction::LI(1, static_cast<u16>(PageDirectory)),
		Instruction::MTP(1),
		Instruction::PGON(),
		Instruction::LUI(2, 0x0080),      // r2 = MappedVA
		Instruction::LI(4, 0x1234),
		Instruction::STR(2, 4, 0),        // *(u32*)MappedVA = 0x1234, through the MMU
		Instruction::LDR(3, 2, 0),        // r3 = *(u32*)MappedVA, through the MMU again
	};
	mapCodeIdentity(m);
	mapPage(m, PageDirectory, DataTable, 2, 0, FrameA, Mmu::PtePresent | Mmu::PteWritable);

	m.step(7);

	CHECK_EQ(m.reg(3), 0x1234u);
	// Not a coincidence: the value actually landed in the physical frame the table points at, not
	// at the virtual address itself (which this machine's 16 MiB default RAM cannot even reach).
	CHECK_EQ(m.memory().readUnchecked<u32>(Address(FrameA)), 0x1234u);
}

TEST(paging, disabled_by_default_addresses_stay_physical)
{
	// No MTP, no PGON: a fresh machine behaves exactly as it did before the MMU existed.
	Machine m{
		Instruction::LI(1, 0x1234),
		Instruction::STR(0, 1, 0x500), // *(u32*)0x500 = 0x1234, base r0 == 0
	};
	m.step(2);

	CHECK_EQ(m.memory().readUnchecked<u32>(Address(0x500)), 0x1234u);
}

TEST(paging, pgoff_returns_to_physical_addressing)
{
	Machine m{
		Instruction::LI(1, static_cast<u16>(PageDirectory)),
		Instruction::MTP(1),
		Instruction::PGON(),
		Instruction::PGOFF(),
		Instruction::LI(2, 0x1234),
		Instruction::STR(0, 2, 0x600), // physical again: base r0 == 0, so this is *(u32*)0x600
	};
	mapCodeIdentity(m);

	m.step(6);

	CHECK(!m.flags().paging());
	CHECK_EQ(m.memory().readUnchecked<u32>(Address(0x600)), 0x1234u);
}

// --- Faults ---------------------------------------------------------------------------------------

TEST(paging, an_unmapped_page_raises_a_page_fault)
{
	Machine m{
		Instruction::LI(1, static_cast<u16>(PageDirectory)),
		Instruction::MTP(1),
		Instruction::PGON(),
		Instruction::LUI(2, 0x0040), // an address whose directory entry was never written: reads as 0, Present clear
		Instruction::LDR(3, 2, 0),
	};
	mapCodeIdentity(m);

	m.step(5);

	// Same shape the existing alignment-fault tests check: the BIOS vector for PageFault points at
	// the default stub, so the PC leaves the program for the BIOS segment.
	CHECK(m.pc().value() >= Memory::BiosSegmentStart.value());
	CHECK(m.pc().value() < Memory::UnrestrictedSegmentStart.value());
}

TEST(paging, a_write_to_a_read_only_page_faults_and_does_not_write)
{
	Machine m{
		Instruction::LI(1, static_cast<u16>(PageDirectory)),
		Instruction::MTP(1),
		Instruction::PGON(),
		Instruction::LUI(2, 0x0080),
		Instruction::LI(4, 0xBEEF),
		Instruction::STR(2, 4, 0), // Present but not Writable: this must fault, not write.
	};
	mapCodeIdentity(m);
	mapPage(m, PageDirectory, DataTable, 2, 0, FrameA, Mmu::PtePresent); // no PteWritable

	m.step(6);

	CHECK(m.pc().value() >= Memory::BiosSegmentStart.value());
	CHECK(m.pc().value() < Memory::UnrestrictedSegmentStart.value());
	CHECK_EQ(m.memory().readUnchecked<u32>(Address(FrameA)), 0u);
}

TEST(paging, a_read_only_page_still_reads)
{
	Machine m{
		Instruction::LI(1, static_cast<u16>(PageDirectory)),
		Instruction::MTP(1),
		Instruction::PGON(),
		Instruction::LUI(2, 0x0080),
		Instruction::LDR(3, 2, 0), // Present, not Writable, but a plain read needs neither.
	};
	mapCodeIdentity(m);
	mapPage(m, PageDirectory, DataTable, 2, 0, FrameA, Mmu::PtePresent); // no PteWritable
	m.memory().writeUnchecked<u32>(Address(FrameA), 0xC0FFEEu);

	m.step(5);

	CHECK_EQ(m.reg(3), 0xC0FFEEu);
	CHECK(m.flags().paging());
}

TEST(paging, mfpf_reports_the_virtual_address_that_faulted)
{
	constexpr Address HandlerAt = Address(0x800); // still inside the identity-mapped first page
	Machine m{
		Instruction::LI(1, static_cast<u16>(PageDirectory)),
		Instruction::MTP(1),
		Instruction::PGON(),
		Instruction::LUI(2, 0x0040), // unmapped, same as the fault test above
		Instruction::LDR(3, 2, 0),
	};
	mapCodeIdentity(m);
	m.installHandler(InterruptNumber::PageFault, HandlerAt, {
		Instruction::MFPF(0),
		Instruction::HALT(),
	});

	m.step(5); // through MTP, PGON, LUI, and the faulting LDR (which redirects PC into the handler)
	m.step(1); // MFPF, inside the handler

	CHECK_EQ(m.reg(0), 0x00400000u);
}

// --- TLB -------------------------------------------------------------------------------------------

TEST(paging, invlpg_makes_a_changed_mapping_visible_immediately)
{
	Machine m{
		Instruction::LI(1, static_cast<u16>(PageDirectory)),
		Instruction::MTP(1),
		Instruction::PGON(),
		Instruction::LUI(2, 0x0080),
		Instruction::LI(4, 0xAAAA),
		Instruction::STR(2, 4, 0),  // frameA[0] = 0xAAAA, through the MMU: this also fills the TLB
		Instruction::LDR(5, 2, 0),  // r5 = 0xAAAA, from the now-cached translation
		Instruction::LDR(6, 2, 0),  // r6: read again *after* the remap below, *before* INVLPG - stale on purpose
		Instruction::INVLPG(2),
		Instruction::LDR(7, 2, 0),  // r7: read *after* INVLPG - must see the new mapping
	};
	mapCodeIdentity(m);
	mapPage(m, PageDirectory, DataTable, 2, 0, FrameA, Mmu::PtePresent | Mmu::PteWritable);

	m.step(7); // through the second LDR (r5), which is also what fills the TLB

	// Simulate a program (or the loader) repointing this virtual page at a different physical frame,
	// without going through the VM at all - exactly what a real OS's page-table edit looks like.
	mapPage(m, PageDirectory, DataTable, 2, 0, FrameB, Mmu::PtePresent | Mmu::PteWritable);
	m.memory().writeUnchecked<u32>(Address(FrameB), 0xBBBBu);

	m.step(1); // r6: the stale TLB entry still points at frameA
	CHECK_EQ(m.reg(6), 0xAAAAu);

	m.step(2); // INVLPG, then r7
	CHECK_EQ(m.reg(7), 0xBBBBu);
}

TEST(paging, flpg_flushes_every_tlb_entry)
{
	Machine m{
		Instruction::LI(1, static_cast<u16>(PageDirectory)),
		Instruction::MTP(1),
		Instruction::PGON(),
		Instruction::LUI(2, 0x0080),
		Instruction::LI(4, 0x1111),
		Instruction::STR(2, 4, 0), // fills the TLB for MappedVA
		Instruction::FLPG(),
		Instruction::LDR(5, 2, 0), // must re-walk rather than trust a flushed entry
	};
	mapCodeIdentity(m);
	mapPage(m, PageDirectory, DataTable, 2, 0, FrameA, Mmu::PtePresent | Mmu::PteWritable);

	m.step(6);
	mapPage(m, PageDirectory, DataTable, 2, 0, FrameB, Mmu::PtePresent | Mmu::PteWritable);
	m.memory().writeUnchecked<u32>(Address(FrameB), 0x2222u);

	m.step(2); // FLPG, then the re-walked read

	CHECK_EQ(m.reg(5), 0x2222u);
}

// --- Control registers -----------------------------------------------------------------------------

TEST(paging, mfp_reads_back_what_mtp_wrote)
{
	Machine m{
		Instruction::LI(1, static_cast<u16>(PageDirectory)),
		Instruction::MTP(1),
		Instruction::MFP(2),
	};
	m.step(3);

	CHECK_EQ(m.reg(2), PageDirectory);
}

TEST(paging, ptbr_is_zero_before_any_mtp)
{
	Machine m{
		Instruction::MFP(2),
	};
	m.step(1);

	CHECK_EQ(m.reg(2), 0u);
}

TEST(paging, a_reset_clears_ptbr_and_disables_paging)
{
	Machine m{
		Instruction::LI(1, static_cast<u16>(PageDirectory)),
		Instruction::MTP(1),
		Instruction::PGON(),
	};
	m.step(3);
	CHECK(m.flags().paging());

	m.vm().engine().reset();

	CHECK(!m.flags().paging());
	CHECK_EQ(m.vm().engine().mmu().ptbr(), 0u);
}

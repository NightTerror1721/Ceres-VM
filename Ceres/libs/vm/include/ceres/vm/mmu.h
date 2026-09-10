#pragma once

#include <ceres/core/isa/address.h>
#include "memory.h"
#include <array>
#include <optional>

namespace ceres::vm
{
	using namespace isa;

	// What the translation was for, so a leaf entry's permission bits can be checked against it.
	// Read is not gated on anything: presence alone makes a page readable, the same way a page
	// with no bits set at all would be unreadable in a "present implies readable" scheme.
	enum class MmuAccess : u8 { Read, Write, Execute };

	// Two-level, 4 KiB-paged translation: a 10/10/12 split over a 32-bit virtual address, the same
	// shape i386 used over 4 GiB of address space backed by far less physical RAM - exactly Ceres's
	// situation, with 1 GiB the most any configuration will ever have behind it.
	//
	// A page directory is always one page (1024 entries * 4 bytes = 4 KiB) and is the only structure
	// that must be resident for paging to be on at all; page tables are the program's own to
	// allocate, one per 4 MiB of virtual address actually mapped, so a small program's translation
	// overhead is kilobytes rather than the 4 MiB a single flat table would cost unconditionally.
	//
	// Directory and leaf entries share one encoding: the top 20 bits are a 4 KiB-aligned physical
	// address (of a page table, or of a frame), and the bottom 12 bits are flags. A directory entry
	// only ever consults Present; permission is a leaf-only concept, exactly as i386 shaped it.
	class Mmu
	{
	public:
		static inline constexpr u32 PageSize = 0x1000; // 4 KiB
		static inline constexpr u32 PageShift = 12;
		static inline constexpr u32 DirectoryEntries = 1024;
		static inline constexpr u32 TableEntries = 1024;

		static inline constexpr u32 PteFrameMask = 0xFFFFF000u;
		static inline constexpr u32 PtePresent    = 1u << 0;
		static inline constexpr u32 PteWritable   = 1u << 1;
		static inline constexpr u32 PteExecutable = 1u << 2;
		static inline constexpr u32 PteAccessed   = 1u << 3; // Set by the MMU the first time a leaf is used.
		static inline constexpr u32 PteDirty      = 1u << 4; // Set by the MMU the first time a leaf is written.

	private:
		// A TLB miss costs two extra physical reads (directory, then table) on top of the access
		// itself; a hit costs a linear scan of a handful of entries, which for a software VM this
		// size is cheaper than hashing would be to set up.
		struct TlbEntry
		{
			bool valid = false;
			u32 vpn = 0;          // address.value() >> PageShift
			u32 frameBase = 0;    // physical, PageSize-aligned
			u32 pteAddress = 0;   // physical address of the leaf entry itself, for the dirty write-back
			bool writable = false;
			bool executable = false;
			bool dirty = false;   // mirrors whether PteDirty has already been written back for this leaf
		};
		static inline constexpr usize TlbSize = 16;

		u32 _ptbr = 0; // Physical address of the page directory. Meaningless until a program sets it.
		std::array<TlbEntry, TlbSize> _tlb{};
		usize _tlbNextVictim = 0;
		Address _faultAddress = Address::Null; // The virtual address MFPF reads back - CR2's equivalent.

	public:
		Mmu() = default;
		Mmu(const Mmu&) = delete;
		Mmu(Mmu&&) = delete;
		~Mmu() = default;

		Mmu& operator=(const Mmu&) = delete;
		Mmu& operator=(Mmu&&) = delete;

	public:
		u32 ptbr() const noexcept { return _ptbr; }

		// A fresh address space shares nothing with the old one, so every cached translation from
		// the old table is simply wrong under the new one - the same reason a real MMU flushes on a
		// CR3 write. Global pages would be the exception; Ceres has none.
		void setPtbr(u32 physicalAddress) noexcept
		{
			_ptbr = physicalAddress;
			invalidateAll();
		}

		Address faultAddress() const noexcept { return _faultAddress; }

		void invalidate(Address virtualAddress) noexcept
		{
			const u32 vpn = virtualAddress.value() >> PageShift;
			for (TlbEntry& entry : _tlb)
			{
				if (entry.valid && entry.vpn == vpn)
				{
					entry.valid = false;
					break;
				}
			}
		}

		void invalidateAll() noexcept
		{
			for (TlbEntry& entry : _tlb)
				entry.valid = false;
			_tlbNextVictim = 0;
		}

		// Back to the state a machine that has never touched paging is in. Called from
		// ExecutionEngine::reset() so RESET leaves no table pointer for the next program to trip over.
		void reset() noexcept
		{
			_ptbr = 0;
			invalidateAll();
			_faultAddress = Address::Null;
		}

	public:
		// Walks (or asks the TLB) for the physical address `virtualAddress` translates to. Returns
		// nullopt on any failure - directory or leaf not present, or a permission the access needs
		// and the leaf does not grant - having already recorded `virtualAddress` as the fault address.
		// The caller (ExecutionEngine) is the one that turns a failure into a PageFault interrupt.
		std::optional<Address> translate(Memory& memory, Address virtualAddress, MmuAccess access) noexcept
		{
			const u32 va = virtualAddress.value();
			const u32 vpn = va >> PageShift;
			const u32 offset = va & (PageSize - 1);

			TlbEntry* hit = nullptr;
			for (TlbEntry& entry : _tlb)
			{
				if (entry.valid && entry.vpn == vpn)
				{
					hit = &entry;
					break;
				}
			}

			if (hit == nullptr)
			{
				hit = walk(memory, va, vpn);
				if (hit == nullptr)
				{
					_faultAddress = virtualAddress;
					return std::nullopt;
				}
			}

			if (access == MmuAccess::Write && !hit->writable)
			{
				_faultAddress = virtualAddress;
				return std::nullopt;
			}
			if (access == MmuAccess::Execute && !hit->executable)
			{
				_faultAddress = virtualAddress;
				return std::nullopt;
			}

			// Dirty is set lazily, the first time a cached leaf is actually written, rather than on
			// every hit - one conditional write-back per page's first write instead of per access.
			if (access == MmuAccess::Write && !hit->dirty)
			{
				const u32 pte = memory.readUnchecked<u32>(Address(hit->pteAddress));
				memory.writeUnchecked<u32>(Address(hit->pteAddress), pte | PteDirty);
				hit->dirty = true;
			}

			return Address(hit->frameBase | offset);
		}

	private:
		// The two-level walk itself, run only on a TLB miss. Present/permission failures return
		// nullptr; the caller turns that into the recorded fault. A successful walk installs (or
		// replaces) a TLB entry and returns a pointer to it.
		TlbEntry* walk(Memory& memory, u32 va, u32 vpn) noexcept
		{
			const u32 dirIndex = (va >> 22) & (DirectoryEntries - 1);
			const u32 tableIndex = (va >> 12) & (TableEntries - 1);

			const Address dirEntryAddress = Address(_ptbr + dirIndex * sizeof(u32));
			const u32 dirEntry = memory.readUnchecked<u32>(dirEntryAddress);
			if (!(dirEntry & PtePresent))
				return nullptr;

			const u32 tableBase = dirEntry & PteFrameMask;
			const Address pteAddress = Address(tableBase + tableIndex * sizeof(u32));
			u32 pte = memory.readUnchecked<u32>(pteAddress);
			if (!(pte & PtePresent))
				return nullptr;

			if (!(pte & PteAccessed))
			{
				pte |= PteAccessed;
				memory.writeUnchecked<u32>(pteAddress, pte);
			}

			TlbEntry& slot = _tlb[_tlbNextVictim];
			_tlbNextVictim = (_tlbNextVictim + 1) % TlbSize;

			slot.valid = true;
			slot.vpn = vpn;
			slot.frameBase = pte & PteFrameMask;
			slot.pteAddress = pteAddress.value();
			slot.writable = (pte & PteWritable) != 0;
			slot.executable = (pte & PteExecutable) != 0;
			slot.dirty = (pte & PteDirty) != 0;

			return &slot;
		}
	};
}

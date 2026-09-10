# Virtual memory and paging

[← Back to index](README.md)

Every address in Ceres was physical until now: `Address` is a bare `u32`, and `Memory` indexes its
backing array with it directly. This page describes the MMU that sits in front of that — two-level,
4 KiB-paged translation, off by default, and fully additive: no existing opcode changes meaning, and
nothing here forces a `.cres` version bump (see [The `.cres` binary format](09-CRES-Binary-Format.md#versioning)).

## Why two levels, and why 4 KiB pages

`Memory::MaxSize` is 1 GiB; the address space `Address` describes is 4 GiB. That fourfold margin is
what makes paging affordable at all, and it is also what settles how many levels the page table
needs.

A single flat table over the whole 4 GiB space, at 4 KiB granularity, needs 2²⁰ entries — 4 MiB,
*fixed*, whether a program uses one page or every one of them. Against the 16 MiB a machine has by
default, that is a quarter of all memory spent on translation before a single instruction of the
actual program runs. Two levels split that into a directory (1024 entries × 4 bytes = 4 KiB, always
resident) and page tables allocated only for the ranges a program actually maps — a small program's
overhead is kilobytes, not megabytes. A third level, the way x86-64 or ARM64 need one, exists to keep
the *top* level small when the address space itself is 48 bits or wider; Ceres's is exactly 32, the
case the classic i386 two-level, 10/10/12 scheme was built for.

| Design | Fixed cost | Typical cost (64 KiB program) | Verdict |
| --- | --- | --- | --- |
| 1 level | 4 MiB | 4 MiB | Too much of a 16 MiB machine, always |
| **2 levels (10/10/12)** | 4 KiB | ~8 KiB | What this page describes |
| 3+ levels | 4 KiB | ~12 KiB | Solves a problem a 32-bit space doesn't have |

## Address layout

```
 31                     22 21                   12 11              0
┌─────────────────────────┬───────────────────────┬─────────────────┐
│   directory index (10)  │   table index (10)     │   offset (12)   │
└─────────────────────────┴───────────────────────┴─────────────────┘
```

```cpp
dirIndex   = (va >> 22) & 0x3FF;
tableIndex = (va >> 12) & 0x3FF;
offset     =  va        & 0xFFF;
```

A directory has 1024 entries pointing at page tables; each table has 1024 entries pointing at 4 KiB
physical frames. Directory and leaf (page-table) entries share one 32-bit encoding: the top 20 bits
are a **4 KiB-aligned** physical address, and the bottom 12 are flags.

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | Present | Cleared means not mapped: any access faults. A directory entry only ever consults this bit. |
| 1 | Writable | A write needs this; a read does not. |
| 2 | Executable | An instruction fetch needs this. |
| 3 | Accessed | Set by the MMU the first time a leaf is used, and never by software. |
| 4 | Dirty | Set by the MMU the first time a leaf is actually written. |

Since `Memory::MaxSize` is 1 GiB, a physical frame number only ever needs 18 bits — the remaining
space in the top 20 is simply unused, the same way a real PTE has room to spare.

**A table's own address, and every frame a leaf points at, must be 4 KiB-aligned** — the low 12 bits
of that field are flags, not part of the address, so an unaligned pointer there is silently truncated
to whatever page it falls inside, not rejected. `@bss`/`@data` declarations have no alignment
attribute today (see [Data types and literals → Alignment](11-Data-Types-and-Literals.md#alignment)),
so a program that builds its own tables has to over-allocate by one page and round the address up at
run time — see the worked example below.

## The seven opcodes

All seven are free-standing: `mtp`/`mfp` and `pgon`/`pgoff` on 0x08–0x0B, `invlpg`/`flpg` on
0x0C–0x0D, `mfpf` on 0x0E — the block right after `sti` (see [Instruction set](05-Instruction-Set.md)),
chosen because it was the first free block and keeps every MMU instruction contiguous.

| Mnemonic | Encoding | Effect |
| --- | --- | --- |
| `mtp rs` | `[rs]` | Page directory base register (PTBR) = `rs`. Also flushes the entire TLB — a new address space shares nothing with the old one. |
| `mfp rd` | `[rd]` | `rd` = PTBR. Zero until the first `mtp`. |
| `pgon` | `[]` | Sets the Paging flag: every subsequent load, store and instruction fetch is translated. |
| `pgoff` | `[]` | Clears it: addresses go straight to physical memory again. |
| `invlpg rs` | `[rs]` | Drops the TLB entry for the page containing the address in `rs`, if any. |
| `flpg` | `[]` | Drops every TLB entry. |
| `mfpf rd` | `[rd]` | `rd` = the virtual address that last faulted — CR2's equivalent. |

`PGON`/`PGOFF` toggle a new `Paging` bit in the flags register (`ExecutionFlag::Paging`), the same
family CLI/STI already live in — see [Registers and flags](03-Registers-and-Flags.md). `RESET` clears
it, along with PTBR and the TLB: a fresh machine is exactly as unpaged as one that has never heard of
the MMU.

## What stays physical no matter what

Three regions never go through translation, whether or not paging is on:

- **The null page and the BIOS** (`0x00000000`–`0x000003FF`, i.e. below `UnrestrictedSegmentStart`
  — see [Memory](02-Memory.md)). Nothing a program's page table describes should need to; leaving
  them out means the BIOS's default fault stub is reachable even from a program whose own tables are
  garbage, the same way a real machine's boot code runs before paging is turned on at all.
- **The system stack** (the top `SystemStackSize` bytes of physical memory, where an interrupt frame
  is saved and restored — see [Memory → The stack](02-Memory.md#the-stack)). This one is load-bearing,
  not just convenient: without it, a page fault taken while the system stack itself happened to be
  unmapped would recurse into dispatching the very fault it was trying to save a frame for.

Everything from `0x00000400` up to the system stack floor is the paged region. A program that turns
paging on has to identity-map (or otherwise map, with Executable set) whatever page it is currently
running out of — the *very next instruction fetch* after `pgon` is already translated, exactly like
enabling paging on real hardware.

## Faults

An MMU failure — not present, or a write/execute against a leaf that doesn't grant it — raises
`InterruptNumber::PageFault` (7), dispatched through `triggerInterrupt` exactly like `AlignmentFault`
or `MemoryFault` already are (see [Interrupts and exceptions](08-Interrupts-and-Exceptions.md)). The
BIOS installs its usual default stub for it, so an unhandled page fault behaves like every other
unhandled fault: print `E`, halt. A real handler reads the faulting address with `mfpf`, the way a
real handler would read CR2. `PageFault` is a reserved name the assembler predefines, so a program
installs one the same way it would for any other fault — see
[Interrupt vector binding](26-Interrupt-Vector-Binding.md):

```casm
interrupt PageFault: page_fault_isr

@text
page_fault_isr:
    mfpf r0        // r0 = the virtual address that faulted
    // ... map it, or don't ...
    iret
```

Because `triggerInterrupt` captures the PC *before* redirecting it, a handler that fixes the mapping
and returns with `iret` re-runs the very instruction that faulted — the standard demand-paging retry
pattern falls out of a mechanism Ceres already had for every other fault.

**A caveat, not a bug**: `pushm`/`popm` stop at the first register a page fault interrupts rather than
continuing through the rest of the mask, and the interrupt-dispatch path itself does not (yet) handle
a fault while saving a fault's own frame beyond the system-stack exemption above — that exemption is
what makes the common case safe, not a claim that Ceres implements a full double-fault mechanism the
way real hardware does.

## The TLB

A miss costs two extra physical reads (directory, then table) on top of the access itself; Ceres
caches the last 16 translations in a small, fully-associative TLB so a hit costs neither. `mtp`
flushes it outright (a new PTBR invalidates everything cached under the old one); `invlpg` drops one
entry, for when a program remaps a single page under its own feet; `flpg` drops all of them without
changing PTBR. **Forgetting to invalidate after editing a live mapping is a real bug class** — the
old translation keeps answering until something evicts or invalidates it, exactly as it would on real
hardware.

## Worked example

`.bss` cannot request 4 KiB alignment, so the buffers below are declared one page larger than they
need and rounded up at run time. This identity-maps the program's own first page — the minimum a
program needs before it can safely call `pgon` at all — and shuts down cleanly through it, which is
what proves the mapping actually took:

```casm
const SYS_CTRL = 0xFF
const EXIT_CODE = 0x01
const PAGE_SIZE = 0x1000

@bss
    // Over-allocated by one page each; round_to_page below finds a 4 KiB-aligned window inside.
    let page_dir_raw: u32[1025]
    let page_table_raw: u32[1025]

@text
global main:
    la r1, page_table_raw
    call round_to_page
    mov r4, r0                 // r4 = page_table, 4 KiB-aligned

    li r2, 0x07                 // Present | Writable | Executable, frame 0 (identity)
    str [r4 + 0], r2

    la r1, page_dir_raw
    call round_to_page
    mov r5, r0                  // r5 = page_dir, 4 KiB-aligned

    or r3, r4, 1                 // page_table | Present
    str [r5 + 0], r3

    mtp r5
    pgon

    // Still running normally: the page just crossed is identity-mapped.
    li r0, EXIT_CODE
    out SYS_CTRL, r0
    halt

// r0 = the next multiple of PAGE_SIZE at or above r1.
round_to_page:
    add r0, r1, PAGE_SIZE - 1
    shr r0, r0, 12
    shl r0, r0, 12
    ret
```

For a mapping that actually moves an address somewhere else — and for what a page fault, a permission
violation, and `invlpg` each look like in practice — see the `paging` suite in
[`test_paging.cpp`](../Ceres/tests/e2e/test_paging.cpp), which builds a non-identity mapping by hand
and exercises every opcode on this page against it.

## What this does not (yet) do

- **No per-page permission enforcement replaces the existing ad hoc checks.** `checkWritable`'s
  text-segment guard and the stack-limit checks in [Memory](02-Memory.md#the-stack) are unaffected by
  paging and keep running alongside it — the MMU's own Writable/Executable bits are a second,
  independent layer, not a replacement.
- **No demand paging, no swap.** Nothing evicts a frame or reuses one a program is still mapping;
  Accessed/Dirty exist for realism and for a future page-replacement policy to consume, not because
  anything in Ceres reads them today.
- **The loader does not build page tables.** A `.cres` loads and runs exactly as it always did, fully
  unpaged, until it builds its own tables and calls `pgon` — see the worked example above.

## Related pages

- [Memory](02-Memory.md) — the physical map paging sits in front of, and the regions that stay
  physical regardless of it.
- [Registers and flags](03-Registers-and-Flags.md) — the `Paging` flag, alongside `Interrupt`,
  `Halting` and `Trap`.
- [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — how `PageFault` is dispatched, and
  how a handler is installed for it instead of falling through to the BIOS's default stub.
- [Instruction set](05-Instruction-Set.md) — the full encoding reference for `mtp`/`mfp`/`pgon`/
  `pgoff`/`invlpg`/`flpg`/`mfpf`.

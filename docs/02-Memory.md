# Memory

[← Back to index](README.md)

The machine's 32-bit physical address space has three things in it (plan/v2 SPEC 2): the **RAM** from
`0x00000000`, up to 2 GiB; the **VRAM** from `0xA0000000`, up to 1 GiB; and the **devices** in the top
16 MiB, from `0xFF000000`. Everything else is empty. The RAM is `ceres::vm::Memory`, a single flat array of
bytes whose every address is a real offset into it; the VRAM is `ceres::vm::Vram`, the same thing at its own
base. By default that is also everything a running program sees: no MMU, no paging, no virtual addressing,
every address physical.

A program that turns paging on (`pgon`) sees something different: `ExecutionEngine` translates most
addresses through the MMU *before* they ever reach `Memory`, which keeps being exactly the flat
physical array described below throughout. See
[Virtual memory and paging](27-Virtual-Memory-and-Paging.md) for how that translation works, and
which addresses (the null page, the BIOS, the system stack) stay physical regardless of it.

## Size

```cpp
static inline constexpr usize DefaultSize = 1024 * 1024 * 64;  // 64 MiB, the standard profile's
static inline constexpr usize MaxSize     = 0x80000000;        // 2 GiB: all of 0x00000000-0x7FFFFFFF
static inline constexpr usize MinSize     = 8192;              // 8 KiB
```

How much RAM and VRAM a machine has is its **profile**'s: `standard`, the default, has 64 MiB of RAM and
32 MiB of VRAM; `micro` has 64 KiB and 32 KiB; `custom` up to 2 GiB and 1 GiB. `ceres run --profile <name>`
picks one, and `--ram <bytes>` or `--vram <bytes>` change it (see
[The machine's clock and its profiles](30-Machine-Clock-and-Profiles.md#profiles)). Both are whole 4 KiB pages:
the RAM from 8 KiB, the VRAM from 16 KiB. SystemControl's `MemorySize` and `VramSize` registers say how much a
machine has.

**Neither costs the host what it does not use.** Both are reserved from the host's operating system as pages
that read as zero and become real memory the first time they are written (`HostPages`, `VirtualAlloc` on Windows
and `mmap` elsewhere). A 2 GiB machine running a small program keeps a few MiB of the host's memory; with the
`std::vector` that used to hold the RAM, a 1 GiB machine took the whole gigabyte before its first instruction.

## The physical map

| Range | Size | Region | A load or store past what is there |
| --- | --- | --- | --- |
| `0x00000000`–`0x7FFFFFFF` | up to 2 GiB | RAM | `MemoryFault`, `FaultReason` `OutOfRam` |
| `0x80000000`–`0x9FFFFFFF` | 512 MiB | empty | `MemoryFault`, `Unmapped` |
| `0xA0000000`–`0xDFFFFFFF` | up to 1 GiB | VRAM | `MemoryFault`, `OutOfVram` |
| `0xE0000000`–`0xFEFFFFFF` | 496 MiB | empty | `MemoryFault`, `Unmapped` |
| `0xFF000000`–`0xFFFFFFFF` | 16 MiB | devices: 256 slots of 64 KiB | see [I/O devices](07-IO-Devices-and-Ports.md) |

The engine routes each access with one comparison against `0xA0000000` and, below it, one against the end of
the RAM, which is as much as it paid when there was only RAM. Everything past that is the slow path: the VRAM
(3 cycles an access where the RAM costs 2), a device register, or the fault, whose `FaultAddress` is the address
the program used and whose `FaultReason` says which of the three it was.

**The VRAM** takes every load and store the RAM does (8, 16, 32 and 64 bits, with the same alignment), the
block instructions and the DMA. Code runs from it too. What it is for arrives with the GPU (plan/v2 F5): for now
it is memory with an address of its own, which every store marks page by page (`Vram::written`) so the GPU's
hardware executor will only have to upload the 4 KiB pages that changed. The debugger shows it with
`vram read` and `vram dump` (see [Debugger](22-Debugger.md)).

**An instruction fetched** from anywhere but the RAM or the VRAM is a `MemoryFault` with the access `Execute`
and the same reasons, and `MmioWidth` in the device window, whose registers are read only by a 32-bit load.

## The RAM map

| Range | Size | Contents |
| --- | --- | --- |
| `0x00000000`–`0x000000FF` | 256 B (`NullPageSegmentSize`) | Interrupt vector table: 64 entries × 4 bytes. Entry 0 doubles as the reset vector, and holds the program's entry point. |
| `0x00000100`–`0x000003FF` | 768 B (`BiosSegmentSize`) | BIOS. A default handler for each vector from `Trap` to `UserInterrupt0` that the program leaves unbound: it shuts the machine down with exit status 1, and `ceres run` says on stderr which exception it was (see [Interrupts](08-Interrupts-and-Exceptions.md)). |
| `0x00000400`– | rest of memory (`UnrestrictedSegmentStart`) | `.text`, `.rodata`, `.data`, `.bss`, then heap and stack. This is where a program actually lives. |

These two constants come straight from [`memory.h`](../Ceres/libs/vm/include/ceres/vm/memory.h):

```cpp
static inline constexpr Address NullPageSegmentStart      = 0_addr;
static inline constexpr Address BiosSegmentStart           = NullPageSegmentStart + Address(0x100);
static inline constexpr Address UnrestrictedSegmentStart   = NullPageSegmentStart + Address(0x400);
```

## Protected vs. unrestricted access

`Memory` exposes two families of accessors:

- **Checked** (`read`, `write`, `readBytes`, `writeBytes`, `peekBytes`, `peekMutBytes`, `setBytes`,
  `copyBytes`): reject any address below `UnrestrictedSegmentStart` (`0x400`). This is what every
  instruction that a `.casm` program can execute goes through, so **a running program cannot
  overwrite the vector table or the BIOS**. The store instructions also check the target before
  they get that far, and raise `MemoryFault` instead of letting the write be silently discarded —
  see [The vector table and the BIOS are read-only](#the-vector-table-and-the-bios-are-read-only).
- **Unchecked** (`readUnchecked`, `writeUnchecked`, `readBytesUnchecked`, …): allow the full address
  range, including the null page. Only internal code uses these — instruction *fetch* (`readInstruction`
  reads through `readUnchecked`), the BIOS initializer, and the interrupt dispatcher, which needs to
  read vector table entries.

### The vector table and the BIOS are read-only

A store whose target overlaps `0x00000000`–`0x000003FF` — `str`, `strb`, `strh`, the float `str`, in
every addressing form — raises `MemoryFault` and is abandoned before it takes effect, exactly as a
store into `.text` is (below). The checked accessors have always refused those addresses, but they
did it by doing nothing, so a program that wrote through a null pointer saw its store "succeed":
the bug surfaced later, somewhere unrelated, or never. A store that straddles the boundary (a word
at `0x3FE`) faults too; one that starts at `0x400` is ordinary memory.

Device block transfers are unchanged: they clamp to unrestricted RAM and report the shortfall in
their count register rather than faulting. A debugger still writes through the unchecked accessors.

### `.text` is read-only

`CeresVM::loadProgram` tells the engine where the code it just placed begins and ends, and every
instruction that writes memory — `str`, `strb`, `strh`, the float `str` — checks the target against
that range first. A write that overlaps it raises
`MemoryFault` and the instruction is abandoned before it takes effect.

This is the fault a lost pointer actually deserves. Overwriting an instruction that has not run
yet used to succeed silently, and the machine then went wrong at whatever address that
instruction lived at — arbitrarily far from the store that caused it.

The range is empty until a program is loaded, and survives a `reset` like the stack limit does.
A debugger still writes through the unchecked accessors, so patching an instruction from the
editor keeps working.

A program's load from the null page or the BIOS reads zero, and a store there that got past the check above
(a push, an interrupt's frame) is dropped. A load or a store **past the end of the RAM** is not quietly zero any
more: it is the `MemoryFault` of [the physical map](#the-physical-map). The byte-range APIs (`writeBytes`,
`readBytes`, …) throw `std::out_of_range` on a bad range; those are only reachable from `IODevice`
implementations and internal code, not from instruction execution.

## Alignment

Sections are laid out on 4-byte boundaries by the linker (see
[Labels and symbols](12-Labels-and-Symbols.md)), and individual variables are padded to their
type's *natural alignment*:

| Type | Alignment |
| --- | --- |
| `u8`, `i8` | 1 |
| `u16`, `i16` | 2 |
| `u32`, `i32`, `f32` | 4 |
| `u64`, `i64`, `f64` | 4 |

A misaligned 16- or 32-bit memory access at run time raises `AlignmentFault`
(see [Interrupts and exceptions](08-Interrupts-and-Exceptions.md)). Byte-sized accesses
(`LDRB`/`STRB`/`LDRSB`) never fault, since there's no alignment requirement narrower than one byte.
Integers are assembled and disassembled byte-by-byte in little-endian order (see `Memory::readRaw`/
`writeRaw`), so the VM behaves identically regardless of the host machine's own byte order.

## The stack

The stack pointer (`r15`/`sp`) is initialized to the top of the **program's own** region on reset
and **grows down** from there. That is not quite the top of memory: the last 4 KiB
(`Memory::SystemStackSize`, 1 KiB until a fault report built at `-O0` needed more) are the system stack, which interrupt handlers run on, so
`ExecutionEngine::reset()` sets `sp = memory.size() - SystemStackSize`, and a handler's starts at
`memory.size()` itself - `0x80000000` on a 2 GiB machine.

A loaded program then starts a little lower: `CeresVM::loadProgram` puts its arguments at the top of
that region, the way a hosted C implementation starts `main`. The strings come first (each
NUL-terminated, the arguments then the `NAME=value` environment), below them the null-terminated
`argv` and `envp` arrays, and `sp` starts at `argv`, 8-aligned. `r0` holds `argc`, `r1` `argv` and `r2`
`envp`, so `main(int argc, char** argv, char** envp)` receives them as its first three arguments; the
system control device's `ArgumentCountRegister`, `ArgumentVectorRegister` and `EnvironmentRegister`
read the same values. With nothing given the block is two null pointers: `argc` is 0.

- Pushing below the **stack limit** raises `StackOverflow` (`hasStackRoom()` in
  `execution_engine.h`). `CeresVM::loadProgram` sets the limit to the address one past the loaded
  image, which it has to walk the sections to find anyway; with no program loaded it stays at
  `UnrestrictedSegmentStart` (`0x400`).
- Popping past where the stack started (i.e., more data popped than was ever pushed) also raises
  `StackOverflow` — same fault class, since a `RET` without a matching `CALL` and an overflow are
  both "the stack is unbalanced".
- **A handler is measured against the system stack instead**, in both directions: while an
  interrupt is being serviced the floor is the top of the program's stack and the ceiling is the
  top of memory. That is what lets a fault be reported when the program's own stack is the thing
  that overflowed — see
  [Interrupts and exceptions](08-Interrupts-and-Exceptions.md).

**The heap too, when the program says where it ends.** Everything between the end of the image and the
stack is free ground, and the limit starts at the bottom of it. A program raises it over its heap
through the system-control device's `StackLimitRegister` (`0xFFFF000C`,
[I/O devices](07-IO-Devices-and-Ports.md#systemcontroldevice-0xffff0000)) each time the heap grows - the
standard library's `malloc` does - and a stack that runs down into the heap then raises `StackOverflow`
rather than flattening allocations. It never goes below the image. A `reset` puts it back at the end of
the image: the program starts over, and its heap with it.

If the `StackOverflow` dispatch cannot fit its own saved flags and PC either, the machine sets the
Trap and Halting flags and stops instead of faulting forever.

## Related pages

- [Registers and flags](03-Registers-and-Flags.md) — `sp`, `fp`, `at` and their roles.
- [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — `AlignmentFault`, `StackOverflow` and when they fire.
- [The `.cres` binary format](09-CRES-Binary-Format.md) — how `.text`/`.rodata`/`.data`/`.bss` get placed inside this map once a program is loaded.
- [The machine's clock and its profiles](30-Machine-Clock-and-Profiles.md#profiles) — how much RAM and VRAM each machine has.
- [Virtual memory and paging](27-Virtual-Memory-and-Paging.md) — a page can map a frame of the RAM, the VRAM or the devices.

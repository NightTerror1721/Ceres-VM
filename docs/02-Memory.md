# Memory

[← Back to index](README.md)

The VM's memory is a single flat array of bytes: `ceres::vm::Memory` wraps a `std::vector<u8>`.
There is no MMU, no paging, and no virtual addressing — every address a program uses is a real
offset into that array.

## Size

```cpp
static inline constexpr usize DefaultSize = 1024 * 1024 * 16; // 16 MiB
static inline constexpr usize MaxSize     = 1024 * 1024 * 1024; // 1 GiB
static inline constexpr usize MinSize     = 1024; // 1 KiB
```

The default machine has 16 MiB of RAM. `ceres run --memory <bytes>` overrides this (see
[CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md)); the constructor rejects sizes outside
`[MinSize, MaxSize]`.

## Memory map

| Range | Size | Contents |
| --- | --- | --- |
| `0x00000000`–`0x000000FF` | 256 B (`NullPageSegmentSize`) | Interrupt vector table: 64 entries × 4 bytes. Entry 0 doubles as the reset vector, and holds the program's entry point. |
| `0x00000100`–`0x000003FF` | 768 B (`BiosSegmentSize`) | BIOS. Currently a 3-instruction stub (`li r0, 'E'`; `out 0x01, r0`; `halt`) that every unhandled fault vector points at. |
| `0x00000400`– | rest of memory (`UnrestrictedSegmentStart`) | `.text`, `.rodata`, `.data`, `.bss`, then heap and stack. This is where a program actually lives. |

These two constants come straight from [`memory.h`](../Ceres-ASM/src/vm/memory.h):

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
  overwrite the vector table or the BIOS**.
- **Unchecked** (`readUnchecked`, `writeUnchecked`, `readBytesUnchecked`, …): allow the full address
  range, including the null page. Only internal code uses these — instruction *fetch* (`readInstruction`
  reads through `readUnchecked`), the BIOS initializer, and the interrupt dispatcher, which needs to
  read vector table entries.

### `.text` is read-only

`CeresVM::loadProgram` tells the engine where the code it just placed begins and ends, and every
instruction that writes memory — `str`, `strb`, `strh`, the float `str`, and the block reads `inm`
and `inrm` — checks the target against that range first. A write that overlaps it raises
`MemoryFault` and the instruction is abandoned before it takes effect.

This is the fault a lost pointer actually deserves. Overwriting an instruction that has not run
yet used to succeed silently, and the machine then went wrong at whatever address that
instruction lived at — arbitrarily far from the store that caused it.

The range is empty until a program is loaded, and survives a `reset` like the stack limit does.
A debugger still writes through the unchecked accessors, so patching an instruction from the
editor keeps working.

Both checked and unchecked reads are "null-page safe" in one more sense: reading past the *end* of
memory, or below the allowed floor, returns zero bytes instead of throwing — the slow path in
`readRaw`/`writeRaw` treats out-of-range bytes as absent rather than raising a C++ exception. Explicit
bounds violations on the byte-range APIs (`writeBytes`, `readBytes`, …) do throw `std::out_of_range`,
though; those are only reachable from `IODevice` implementations and internal code, not from
instruction execution.

## Alignment

Sections are laid out on 4-byte boundaries by the linker (see
[Labels and symbols](12-Labels-and-Symbols.md)), and individual variables are padded to their
type's *natural alignment*:

| Type | Alignment |
| --- | --- |
| `u8`, `i8` | 1 |
| `u16`, `i16` | 2 |
| `u32`, `i32`, `f32` | 4 |

A misaligned 16- or 32-bit memory access at run time raises `AlignmentFault`
(see [Interrupts and exceptions](08-Interrupts-and-Exceptions.md)). Byte-sized accesses
(`LDRB`/`STRB`/`LDRSB`) never fault, since there's no alignment requirement narrower than one byte.
Integers are assembled and disassembled byte-by-byte in little-endian order (see `Memory::readRaw`/
`writeRaw`), so the VM behaves identically regardless of the host machine's own byte order.

## The stack

The stack pointer (`r15`/`sp`) is initialized to the top of the **program's own** region on reset
and **grows down** from there. That is not quite the top of memory: the last kilobyte
(`Memory::SystemStackSize`) is the system stack, which interrupt handlers run on, so
`ExecutionEngine::reset()` sets `sp = memory.size() - SystemStackSize`.

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

A `reset` leaves the limit where it is: the same image is still in memory, so the same ground is
still worth guarding.

**What it still does not protect:** the heap. Everything between the end of the image and the
stack is free ground, and the limit sits at the bottom of it, so a stack that runs all the way
down will have flattened whatever a heap put there first — it just cannot reach the program's own
code, data or `.bss` any more.

If the `StackOverflow` dispatch cannot fit its own saved flags and PC either, the machine sets the
Trap and Halting flags and stops instead of faulting forever.

## Related pages

- [Registers and flags](03-Registers-and-Flags.md) — `sp`, `fp`, `at` and their roles.
- [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — `AlignmentFault`, `StackOverflow` and when they fire.
- [The `.cres` binary format](09-CRES-Binary-Format.md) — how `.text`/`.rodata`/`.data`/`.bss` get placed inside this map once a program is loaded.

# The `.cres` binary format

[← Back to index](README.md)

`ceres asm source.casm -o program.cres` writes a `vm::Program` out to disk. The format is defined by
`ProgramHeader` in [`program.h`](../Ceres/libs/core/include/ceres/core/format/program.h) and implemented in
[`program.cpp`](../Ceres/libs/core/src/format/program.cpp).

## File layout

A `.cres` file is the header, byte-packed, followed immediately by the three section buffers back to
back:

```
┌──────────────────────────┐
│ ProgramHeader (32 B)     │
├──────────────────────────┤
│ .text   (textSize)       │
├──────────────────────────┤
│ .rodata (rodataSize)     │
├──────────────────────────┤
│ .data   (dataSize)       │
├──────────────────────────┤
│ interrupt vectors (opt.) │
├──────────────────────────┤
│ debug section (opt.)     │
└──────────────────────────┘
```

The interrupt vector section is present only when `flags` has bit 1 set, which happens automatically
whenever the source declares at least one `interrupt NUMBER: handler` binding. It is a little-endian
`u32` count, then that many 5-byte `{u8 interruptNumber, u32 handlerAddress}` entries — see
[Interrupt vector binding](26-Interrupt-Vector-Binding.md#storage-in-cres) for why it is a sparse
patch list rather than the full 64-entry table, and how the loader applies it.

The debug section is present only when `flags` has bit 0 set, which `ceres asm --debug` does. Its
own version is 2, which added a frame table — where each function begins and ends, and whether it
opens a frame pointer with `enter`. A version 1 section still reads: the field the frame count
lives in was a reserved zero, which means "no frames". It is
a little-endian `u32` byte count followed by that many bytes, so a reader with no interest in it can
skip it without knowing its layout — and a reader that predates it never looks past `.data` at all.
See [Debug information](21-Debug-Information.md).

`.bss` is **not** present in the file at all — it occupies no space on disk, exactly like an ELF
`.bss` section. The VM allocates and zero-fills it when the program is loaded.

## `ProgramHeader`

```cpp
#pragma pack(push, 1)
struct ProgramHeader
{
    static inline constexpr u32 MagicNumber    = 0x43524553; // 'CRES' in ASCII
    static inline constexpr u16 CurrentVersion = 3;
    static inline constexpr u16 MinimumSupportedVersion = 3;

    u32 magic;
    u16 version;
    u16 flags;
    u32 entryPoint;

    u32 textSize;
    u32 rodataSize;
    u32 dataSize;
    u32 bssSize;

    u32 minimumStack;
};
#pragma pack(pop)
```

| Field | Meaning |
| --- | --- |
| `magic` | Always `0x43524553` — the ASCII bytes `CRES`. Loading rejects any file that doesn't start with this. |
| `version` | Format version; currently `3`. A file below `MinimumSupportedVersion` is rejected — see [Versioning](#versioning). |
| `flags` | Bit 0 (`ProgramFlags::HasDebugInfo`) says a debug section follows; bit 1 (`ProgramFlags::HasInterruptVectors`) says an interrupt vector patch table does, immediately before it. Every other bit is still reserved. See [Debug information](21-Debug-Information.md) and [Interrupt vector binding](26-Interrupt-Vector-Binding.md). |
| `entryPoint` | The absolute address execution starts at (the resolved address of the `main` label). |
| `textSize` / `rodataSize` / `dataSize` / `bssSize` | Byte sizes of each section, as computed by the linker (each rounded up to a 4-byte boundary — see [Labels and symbols](12-Labels-and-Symbols.md)). |
| `minimumStack` | Reserved; not currently populated with a meaningful value by the emitter. |

The struct is `#pragma pack(push, 1)`, so there's no implicit padding between fields — the on-disk
layout matches the struct member order exactly, 32 bytes total.

## Versioning

```cpp
if (header.version > ProgramHeader::CurrentVersion ||
    header.version < ProgramHeader::MinimumSupportedVersion)
    return std::unexpected("Unsupported .cres version in file: ...");
```

Both ends of the range are checked, and the lower one is the interesting half.

The check originally only rejected files **newer** than the current version, and accepted every older
one. That was fine while the instruction encoding never changed. Twice now it has:

| Change | What the same bytes used to mean |
| --- | --- |
| **1 → 2** | The comparison jumps needed sixteen opcodes where eight slots were free, so the control-flow block grew and pushed the three families above it up (see [Instruction set → The opcodes moved](05-Instruction-Set.md#the-opcodes-moved)). `0x70` was `PUSH`; it is now `JAB`. |
| **2 → 3** | Memory displacements became **signed**. A v2 `[r1 + 65528]` was the only way to write what is now `[r1 - 8]`, so the same sixteen bits address different memory. |

Neither is detectable from the file itself. Without a lower bound an old file would load cleanly and
run as something else entirely, with no error and no trace — a `PUSH` executed as a conditional jump,
or a load reaching 64 KiB in the wrong direction.

```
$ ceres run old.cres
Failed to load old.cres: Unsupported .cres version in file: old.cres
(this build reads versions 2 to 2; reassemble the source)
```

The fix is always to reassemble from source; there is no converter, and there is no reason to want
one — the `.casm` is the artefact worth keeping.

Note the contrast with the debug section, which is announced by a **flag** rather than a version
bump precisely so that older builds keep reading files they can in fact run (see
[Debug information](21-Debug-Information.md)). A flag is right when the change is additive; a version
is right when the meaning of existing bytes changes.

## Loading a program

`Program` offers several ways to load the same format, all converging on the same parsing logic:

```cpp
static std::expected<Program, std::string> loadFromFile(const std::filesystem::path& filePath);
static std::expected<Program, std::string> loadFromBytes(std::span<const ByteType> bytes);
static std::expected<Program, std::string> loadFromStream(std::istream& stream);
static std::expected<Program, std::string> loadFromMemory(const void* memory, usize size);
static std::expected<Program, std::string> loadFromString(const std::string& str);
```

`ceres run program.cres` and `ceres disasm program.cres` both go through `loadFromFile`. When the
input to `ceres run`/`ceres disasm` is a `.casm` file instead, no `.cres` round-trip happens at all —
the assembler's in-memory `Program` is handed directly to the VM (see `main.cpp`'s `loadProgram()`
helper, which dispatches purely on file extension).

## From `.cres` to live memory

Once loaded, `CeresVM::loadProgram()` (see [`ceresvm.cpp`](../Ceres/libs/vm/src/ceresvm.cpp)) places
the sections into the machine's actual memory, starting right after the BIOS at
`Memory::UnrestrictedSegmentStart` (`0x400`):

```
0x00000400  .text    (textSize bytes,   from the file)
   ...      .rodata  (rodataSize bytes, from the file)
   ...      .data    (dataSize bytes,   from the file)
   ...      .bss     (bssSize bytes,    zero-filled, not present in the file)
   ...      heap / stack (whatever memory remains, up to Memory's total size)
```

This mirrors exactly the memory map the linker computed at assembly time (see
[Labels and symbols → Linking](12-Labels-and-Symbols.md#linking-multiple-translation-units)) — every
absolute address baked into the binary during assembly (labels, variable addresses, `la`/`ldv`/`stv`
targets) is only valid because the loader places sections at these same offsets every time. There is
no relocation step at load time; the linker already did all address fixups once, at assembly time.

Right after that, `loadProgram()` patches any bound interrupt vectors into the null page — after the
BIOS has installed its default stub table, so a vector nothing bound still falls through to that
stub unchanged. See [Interrupt vector binding](26-Interrupt-Vector-Binding.md).

The program counter is then set to `header.entryPoint`, and the stack pointer to the top of the
machine's memory (see [Memory](02-Memory.md#the-stack)) — note this means the *usable* stack size
depends on how much memory the host allocated (`ceres run --memory <bytes>`), not on anything stored
in the `.cres` file itself.

## Related pages

- [Memory](02-Memory.md) — the full memory map this format is placed into.
- [CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md) — how `ceres asm`/`run`/`disasm` decide which path to take.
- [Debug information](21-Debug-Information.md) — the optional trailing section and why it is a flag rather than a version bump.
- [Labels and symbols](12-Labels-and-Symbols.md) — how the linker computes the sizes stored in the header.
- [Interrupt vector binding](26-Interrupt-Vector-Binding.md) — the other optional section, and how the loader applies it.

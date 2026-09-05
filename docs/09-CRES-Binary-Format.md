# The `.cres` binary format

[← Back to index](README.md)

`ceres asm source.casm -o program.cres` writes a `vm::Program` out to disk. The format is defined by
`ProgramHeader` in [`program.h`](../Ceres-ASM/src/vm/program.h) and implemented in
[`program.cpp`](../Ceres-ASM/src/vm/program.cpp).

## File layout

A `.cres` file is the header, byte-packed, followed immediately by the three section buffers back to
back:

```
┌─────────────────────┐
│ ProgramHeader (32 B) │
├─────────────────────┤
│ .text   (textSize)   │
├─────────────────────┤
│ .rodata (rodataSize) │
├─────────────────────┤
│ .data   (dataSize)   │
└─────────────────────┘
```

`.bss` is **not** present in the file at all — it occupies no space on disk, exactly like an ELF
`.bss` section. The VM allocates and zero-fills it when the program is loaded.

## `ProgramHeader`

```cpp
#pragma pack(push, 1)
struct ProgramHeader
{
    static inline constexpr u32 MagicNumber    = 0x43524553; // 'CRES' in ASCII
    static inline constexpr u16 CurrentVersion = 1;

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
| `version` | Format version; currently always `1`. |
| `flags` | Reserved for future use; currently unused by the loader. |
| `entryPoint` | The absolute address execution starts at (the resolved address of the `main` label). |
| `textSize` / `rodataSize` / `dataSize` / `bssSize` | Byte sizes of each section, as computed by the linker (each rounded up to a 4-byte boundary — see [Labels and symbols](12-Labels-and-Symbols.md)). |
| `minimumStack` | Reserved; not currently populated with a meaningful value by the emitter. |

The struct is `#pragma pack(push, 1)`, so there's no implicit padding between fields — the on-disk
layout matches the struct member order exactly, 32 bytes total.

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

Once loaded, `CeresVM::loadProgram()` (see [`ceresvm.cpp`](../Ceres-ASM/src/vm/ceresvm.cpp)) places
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

The program counter is then set to `header.entryPoint`, and the stack pointer to the top of the
machine's memory (see [Memory](02-Memory.md#the-stack)) — note this means the *usable* stack size
depends on how much memory the host allocated (`ceres run --memory <bytes>`), not on anything stored
in the `.cres` file itself.

## Related pages

- [Memory](02-Memory.md) — the full memory map this format is placed into.
- [CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md) — how `ceres asm`/`run`/`disasm` decide which path to take.
- [Labels and symbols](12-Labels-and-Symbols.md) — how the linker computes the sizes stored in the header.

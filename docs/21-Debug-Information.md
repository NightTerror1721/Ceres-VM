# Debug information

[← Back to index](README.md)

The assembler already knows where every source line ends up: `RelocatableStatement` carries the
file, the line and the final address of each statement, and the linker resolves every symbol to an
absolute address before a single byte is emitted. Until this existed, all of that was discarded the
moment `assemble()` returned.

`ceres asm --debug` keeps it, as two tables: a **line table** mapping addresses to source locations
in both directions, and a **symbol table** giving every label, variable and constant its address,
type and size. Both are produced by
[`DebugInfoBuilder`](../Ceres/libs/core/include/ceres/core/format/debug_info.h) during the same walk of the linked AST
that the emitter uses to write bytes, so nothing is re-analysed.

## Why the addresses are directly usable

There is **no relocation at load time**. The linker assigns final addresses at assembly time and
`CeresVM::loadProgram()` always places the sections at the same offsets (see
[The `.cres` binary format](09-CRES-Binary-Format.md)). An address in the line table is therefore
the address the program counter will actually hold — no base to add, no image to slide.

## Using it

```bash
ceres asm program.casm --debug --listing        # listing annotated with source lines
ceres asm program.casm --debug -o program.cres  # tables appended to the .cres
ceres asm program.casm --emit-debug-json        # the tables as JSON on stdout
ceres disasm program.cres --debug               # read back what the file carries
```

The annotated listing adds a fourth column:

```
00000400  43100000  LUI r1, 0                   ; main.casm:10
00000404  33110468  ORI r1, r1, 1128            ; main.casm:10
00000408  6100002c  CALL +44                    ; main.casm:11
```

Both words of the `la r1, HELLO_MSG` on line 10 report line 10 — see *One line, several addresses*
below.

## The line table

One entry per **machine word written to `.text`**, sorted by address.

| Field | Meaning |
| --- | --- |
| `address` | Absolute address of the word. |
| `fileId` / `line` | Where the instruction was **written**. For a macro-expanded instruction this is inside the macro's body. |
| `expansionFileId` / `expansionLine` | Where the **user's own code** is. Equal to `fileId`/`line` for anything written by hand. |
| `column` | Reserved; statements only carry line numbers today, so it is always 0. |
| `macroDepth` | 0 for hand-written code, 1 for a macro's expansion, 2 for a macro called from a macro, and so on. |
| `flags` | `FirstOfLine`, `MacroExpansion`, `PseudoPadding`. |

### Two locations, and which one to use

A macro-expanded instruction has two honest answers to "where is this from". Stepping and
breakpoints use the **expansion site**: a debugger that stepped through `fileId`/`line` instead
would bounce the cursor back into the macro's definition on every expanded instruction, which is
almost never what the programmer wants to see. `fileId`/`line` remain available for "step into the
macro" and for showing the macro body itself.

```casm
macro proc_enter
    push r4          // line 24  <- where it was written
    push r5          // line 25
endmacro

ask_yes_no:
    proc_enter       // line 58  <- where the programmer is
```

Both `push` words report `line = 24` and `25`, `expansionLine = 58`, `macroDepth = 1`.

For a macro invoked from inside another macro, the expansion site propagates from the **outermost**
call, so the location is always somewhere the programmer actually typed.

### One line, several addresses

A pseudo-instruction expands to two or three words (`la` → `lui` + `ori`; `ldv`/`stv` → `lui` +
`ori` + a load or store, see [Pseudo-instructions](06-Pseudo-Instructions.md)), and a macro call
expands to as many as its body has. All of them share one expansion line.

Only the first word of each such run carries `FirstOfLine`. That is where a line breakpoint belongs
and where a source-level step should stop; stopping on every word of the run would make one
`ldv` look like three steps through the same line.

`PseudoPadding` marks a `NOP` the emitter wrote to fill out the size the assembler reserved before
it knew which overload would be chosen. No mnemonic in the current instruction set actually pads —
every overload of `la`, `ldv` and `stv` happens to be the same length — but the emitter can produce
it, so the flag exists and stepping should pass straight through.

### Queries

```cpp
std::optional<SourceLocation> locationOf(u32 address) const;              // where am I stopped?
std::vector<u32>              addressesOf(std::string_view f, u32 line);  // every word of a line
std::optional<u32>            firstAddressOfLine(std::string_view f, u32 line); // where a breakpoint goes
const SymbolEntry*            symbolContaining(u32 address) const;
const SymbolEntry*            symbolNamed(std::string_view name) const;
```

`locationOf` resolves an address that is not on an instruction boundary to the instruction
containing it, but only within one word — an address past the end of `.text` is not "inside the
last instruction".

### Matching paths

The table records paths exactly as the assembler was given them, which keeps builds reproducible
but means an editor sending an absolute path may not match a unit assembled from a relative one.
`resolveFileId` tries, in order: the string as-is, the lexically normalised form, and finally the
file name alone — that last one only when exactly one recorded file bears it, so the answer is
never a guess between two same-named files in different directories.

## The symbol table

One entry per label, variable and constant, sorted by address and then by name (the assembler's own
symbol table is an `unordered_map`, whose iteration order would otherwise make the output differ
between runs).

| Field | Meaning |
| --- | --- |
| `nameOffset` | Into the string blob. Local labels are stored qualified: `main.loop`. |
| `address` | Absolute, already relocated. 0 for a constant, which has none. |
| `size` | Bytes occupied. 0 for labels and constants. |
| `elementCount` | 1 for a scalar, N for an array, 0 for an unsized one. |
| `scalarType` | `u8`…`f32`, or invalid for a label. |
| `value` | Raw bits of a scalar constant's value; valid when the `HasValue` flag is set. |
| `kind` / `section` / `flags` | Label/constant/variable, text/rodata/data/bss/none, global/readonly/has-value. |

A constant occupies no memory, so it is recorded with no section and no address — but with its
value, which is what a watch expression will want.

## On disk

The tables are serialised into a self-describing block appended **after** the data section:

```
┌──────────────────────┐
│ ProgramHeader (32 B)  │  flags bit 0 = HasDebugInfo
├──────────────────────┤
│ .text                 │
│ .rodata               │
│ .data                 │
├──────────────────────┤
│ u32 length (LE)       │  written by Program, so it can skip the block without reading it
├──────────────────────┤
│ CDBG header (32 B)    │  magic, version, total size, table counts
│ file table            │  u32 offsets into the string blob
│ line table            │  28 bytes per entry
│ symbol table          │  24 bytes per entry
│ string blob           │  NUL-separated paths and symbol names
└──────────────────────┘
```

Everything inside the block is written byte by byte in little-endian, so a `.cres` carrying debug
information means the same thing whatever the host's byte order is — the same rule the instruction
encoder already follows.

### Why a flag and not a version bump

`ProgramHeader::version` was already checked as `version > CurrentVersion → reject`. Raising it
would make every `ceres` built before this change refuse a file it can in fact run perfectly well.
`flags` was reserved and unused, so bit 0 announces the block instead, and a build that does not
know the bit simply never reads past the data section.

That compatibility is real, not aspirational: every existing loader reads exactly the sizes the
header declares and stops (`loadFromBytes` and `loadFromMemory` check `size < expectedSize`, not
`!=`), so the bytes an older reader consumes are identical with and without `--debug` — the flags
word is the one difference. `tests/test_debug_info.cpp` asserts exactly that.

A block this build cannot parse is **dropped with a warning rather than failing the load**: nothing
needs it to run the program, so a truncated or foreign debug section costs you debugging, not
execution.

### Two paths, one of which never serialises

`ceres run program.casm` and a future `ceres debug program.casm` assemble in memory and hand the
`Program` straight to the VM — the debug information travels as a live object next to it and is
never written out. Serialisation only matters for `.cres` files, that is, for "build here, debug
there".

## Cost

Debug information is off unless asked for. On `examples/main.casm` it adds 1220 bytes to a 152-byte
program — the tables are dominated by paths and symbol names, so the ratio improves sharply with
program size, but a release build should simply not pass `--debug`.

## Related pages

- [The `.cres` binary format](09-CRES-Binary-Format.md) — the header and section layout this extends.
- [Pseudo-instructions](06-Pseudo-Instructions.md) — why one line can be three addresses.
- [Macros](14-Macros.md) — why an instruction has two source locations.
- [CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md) — where the tables are built.

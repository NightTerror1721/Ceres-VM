# Labels and symbols

[← Back to index](README.md)

## Label levels

```casm
global main:      // Global — exported for other translation units to reference
helper:           // File — visible within this file, not exported
.loop:             // Local — scoped to the nearest preceding non-local label
```

`LabelLevel` (in [`common_defs.h`](../Ceres-ASM/src/assembler/common_defs.h)) has exactly three
values:

| Level | Written as | Visible from |
| --- | --- | --- |
| `Global` | `global name:` | Any translation unit in the assembly (subject to the one-definition rule below). |
| `File` | `name:` | The file that defines it, including code that comes *before* the label — labels aren't order-dependent within a file. |
| `Local` | `.name:` | Only within the same file, and only between this local label's parent and the next non-local label. |

## Local labels are namespaced by their parent

A local label `.loop` is internally stored as `parent.loop`, where `parent` is the name of the
nearest preceding `global`/file-level label. This is exactly what lets every subroutine define its
own `.loop` without clashing:

```casm
strlen:
.loop:              // stored as "strlen.loop"
    ...
    jp .loop

print:
.loop:              // stored as "print.loop" — no collision with strlen's
    ...
```

`.loop` used as an *operand* (not a declaration) always resolves against the label of the statement
that references it, not the file as a whole — `TranslationUnitBuilder` tracks
`_lastParentLabel` as it walks the file, updating it every time it processes a non-local label
statement (see [`translation_unit.cpp`](../Ceres-ASM/src/assembler/translation_unit.cpp)). A local
label reference before *any* non-local label has been seen in the file is invalid.

## The `main` entry point

`SymbolTable::EntryPointLabelName` is literally the string `"main"`. There is no other mechanism to
declare an entry point — the linker looks up whatever symbol is named `main` (which must be declared
`global`) and its resolved address becomes `ProgramHeader::entryPoint` (see
[The `.cres` binary format](09-CRES-Binary-Format.md)).

## Symbols: labels, constants, and variables

`SymbolTable` (in [`symbol_table.h`](../Ceres-ASM/src/assembler/symbol_table.cpp)) stores three kinds
of symbol, all keyed by name in one map per translation unit:

| `SymbolType` | Created by | Carries |
| --- | --- | --- |
| `Label` | A label declaration (`name:`, `global name:`, `.name:`) | Section + address |
| `Constant` | `const NAME = value` | A resolved `LiteralValue`, no address (constants occupy no memory) |
| `Variable` | `let NAME: type = value` / `let NAME: type` | Section + address, a `DataType`, optionally an initial `LiteralValue`, and a read-only flag (set for anything declared in `@rodata`) |

Redefining a symbol name within the same translation unit is a hard error
(`checkRedefinition`) — you cannot shadow a label with a variable, or redeclare the same constant
twice, even with an identical value.

## How a name is resolved

When the translation-unit builder processes an instruction's operands, it tries to resolve every
identifier operand immediately, in this order (`SymbolTable::resolveOperand`, called via
`tryResolveOperand` at this stage):

1. Look it up in this file's own symbol table (any level: global, file, or, if the reference is
   local, scoped to the current parent label).
2. If not found, record it as an **unresolved symbol** (`UnresolvedSymbol { name, parentName, line }`)
   for the linker to try again later — this is what lets a file reference a `global` label declared
   in a different translation unit, or, for that matter, a label declared *later* in the same file
   (file-level labels aren't required to appear before their first use).

A local reference (`.name`) that never resolves — neither in this pass nor at link time — is reported
as `Linker error: Unresolved local symbol '.{name}'`; anything else unresolved is reported as
`Linker error: Unresolved symbol '{name}'`.

## Linking multiple translation units

`Linker::link()` (in [`linker.cpp`](../Ceres-ASM/src/assembler/linker.cpp)) runs in three passes over
every translation unit that went into the assembly:

1. **Compute the overall memory map.** Each unit's `.text`/`.rodata`/`.data`/`.bss` sizes are summed,
   with every unit's contribution individually rounded up to a 4-byte boundary first (`alignUp`,
   `SectionAlignment = 4`) — so the *next* unit's section always starts aligned too, not just each
   unit's own variables. The four section base addresses in the final image are laid out
   contiguously in a fixed order: `.text`, then `.rodata`, then `.data`, then `.bss`, starting right
   after the BIOS at `Memory::UnrestrictedSegmentStart`.
2. **Relocate every unit's own symbols** to their final addresses (each unit initially computed
   addresses relative to `0` for its own sections; the linker adds each unit's cumulative offset into
   the shared image) and merge every `global` symbol into one shared global symbol table. A `global`
   symbol with the same name defined in two different translation units is reported as
   `Linker error: Symbol '{name}' is defined in multiple translation units.` — global names must be
   unique across the whole program.
3. **Re-walk every unit's instructions**, resolving any operand that's still an unresolved identifier
   against the now-complete global symbol table, and validating that the resulting instruction (now
   with concrete operand types) actually matches a known signature in
   [`instruction_info.cpp`](../Ceres-ASM/src/assembler/instruction_info.cpp) — an instruction that
   only becomes invalid once its operand is resolved (e.g. referencing a label where a variable was
   expected) is caught right here.

Note that **file-level and local symbols are never merged into the global table** — only symbols
declared `global` cross translation-unit boundaries. Two different files can each have their own
`helper:` (file-level) label without conflict.

## Related pages

- [Constants and expressions](13-Constants-and-Expressions.md) — constants live in the same symbol table as labels/variables.
- [Modules and `import`](15-Modules-and-Import.md) — how symbols and macros travel between files that `import` each other.
- [The `.cres` binary format](09-CRES-Binary-Format.md) — the section sizes/entry point the linker computes.
- [CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md) — where linking sits in the overall build.

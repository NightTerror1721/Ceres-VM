# Labels and symbols

[← Back to index](README.md)

## Label levels

```casm
global main:      // Global — exported for other translation units to reference
helper:           // File — visible within this file, not exported
.loop:             // Local — scoped to the nearest preceding non-local label
```

`LabelLevel` (in [`common_defs.h`](../Ceres/libs/asm/include/ceres/asm/common_defs.h)) has exactly three
values:

| Level | Written as | Visible from |
| --- | --- | --- |
| `Global` | `global name:` | Any translation unit in the assembly (subject to the one-definition rule below). |
| `File` | `name:` | The file that defines it, including code that comes *before* the label — labels aren't order-dependent within a file. |
| `Local` | `.name:` | Only within the same file, and only between this local label's parent and the next non-local label. |

## `global` applies to every declaration, not just labels

A label was once the only thing the language could mark. Now the same prefix governs constants,
variables, macros, structs and register aliases, and the rule is uniform:

```casm
global const MAX_PLAYERS = 4    // exported
const INTERNAL_SLACK    = 8     // private to this file

@data
    global let scoreboard: u32[8]   // exported, address and all
    let scratch:           u32[8]   // private

global macro print_char $reg, $code // exported
    ...
endmacro

global struct Entity                // exports every offset constant it generates
    ...
endstruct

global alias frame_base = r8    // exported; without `global`, file-scoped like the rest
```

**Nothing without `global` leaves the file that declares it.** A name a module declares privately is
reported as exactly that rather than as merely unresolved:

```
'MAX_PLAYERS' is declared in 'lib/rules.casm' but is not global, so it is not visible here.
```

Two kinds of symbol behave differently once exported, and the difference matters:

| Kind | How it reaches another file |
| --- | --- |
| Labels and variables | Through the **linker's global symbol table**. They have addresses, so they have to be linked; defining the same global one twice is a linker error. |
| Constants (including struct offsets) and macros | Through **`import`**. They occupy no memory and are substituted at their point of use, so there is nothing to link. |

That is why two independent libraries can each declare `global const MAX` without colliding: neither
is published to the linker. The clash is only reported if some file imports both *and* uses the name
— see [Modules and import](15-Modules-and-Import.md#when-two-modules-export-the-same-name).

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
statement (see [`translation_unit.cpp`](../Ceres/libs/asm/src/translation_unit.cpp)). A local
label reference before *any* non-local label has been seen in the file is invalid.

## The `main` entry point

`SymbolTable::EntryPointLabelName` is literally the string `"main"`. There is no other mechanism to
declare an entry point — the linker looks up whatever symbol is named `main` (which must be declared
`global`) and its resolved address becomes `ProgramHeader::entryPoint` (see
[The `.cres` binary format](09-CRES-Binary-Format.md)).

## Symbols: labels, constants, and variables

`SymbolTable` (in [`symbol_table.h`](../Ceres/libs/asm/src/symbol_table.cpp)) stores three kinds
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

`Linker::link()` (in [`linker.cpp`](../Ceres/libs/asm/src/linker.cpp)) runs in three passes over
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
   [`instruction_info.cpp`](../Ceres/libs/asm/src/instruction_info.cpp) — an instruction that
   only becomes invalid once its operand is resolved (e.g. referencing a label where a variable was
   expected) is caught right here.

Note that **file-level and local symbols are never merged into the global table** — only symbols
declared `global` cross translation-unit boundaries. Two different files can each have their own
`helper:` (file-level) label without conflict.

Global *constants* are deliberately not merged either, for the reason given above: they have no
address to link, and publishing them would make two libraries collide over a name neither file
necessarily uses.

## Symbols the linker defines

Section addresses are not knowable from any one file: they depend on every unit in the link, so no
amount of `sizeof` over what a file declares adds up to where that file ended up. Nine labels carry
the answer, inserted into the global symbol table once the layout is fixed:

| Symbol | Address |
| --- | --- |
| `__text_start` | Where `.text` begins — always `0x400` today |
| `__text_end` | One past the last byte of `.text` |
| `__rodata_start` / `__rodata_end` | The bounds of `.rodata` |
| `__data_start` / `__data_end` | The bounds of `.data` |
| `__bss_start` / `__bss_end` | The bounds of `.bss` |
| `__heap_start` | The first free byte above the program |

`__heap_start` is the same address as `__bss_end`. It exists under its own name because that is the
question a program actually asks: everything from there up is free ground, with the stack growing
down to meet it — and the stack now [faults](02-Memory.md#the-stack) rather than growing through the
image, so the two really do share that space.

```casm
@text
global main:
    la r1, __heap_start         // where a bump allocator starts
    mov r2, sp                  // and where it must stop
```

They are ordinary global labels, so `la` reaches them and nothing else about them is special.
Declaring one yourself is reported as such:

```
Linker error: '__heap_start' is defined by the linker, so a program cannot declare it.
```

There is deliberately **no `__stack_top`**. The stack pointer starts at the size of memory, which is
a property of the machine — `--memory` picks it at run time, long after the link. A program that
wants it reads `sp` on entry, before anything has pushed.

## Unused private symbols

Because a private symbol provably cannot be reached from outside its file, one that nothing inside
the file names either is dead with certainty — and is reported as a warning:

```
warning [game.casm:4] 'SLACK' is declared but never used, and is not global, so nothing
                      outside this file can use it either
```

Labels are exempt (one nothing jumps to is often an entry point or a table marker), and so are
struct field offsets, which are generated rather than written. See
[Errors and diagnostics](17-Errors-and-Diagnostics.md#warnings).

## Related pages

- [Constants and expressions](13-Constants-and-Expressions.md) — constants live in the same symbol table as labels/variables.
- [Modules and `import`](15-Modules-and-Import.md) — how symbols and macros travel between files that `import` each other.
- [The `.cres` binary format](09-CRES-Binary-Format.md) — the section sizes/entry point the linker computes.
- [CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md) — where linking sits in the overall build.
- [Structs](23-Structs.md) — a `global struct` and the constants it exports.

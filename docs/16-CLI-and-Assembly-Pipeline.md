# CLI and assembly pipeline

[← Back to index](README.md)

## Building the `ceres` binary

```bash
cd Ceres-ASM/src
g++ -std=c++23 -I. -o ceres main.cpp vm/*.cpp assembler/*.cpp debug/*.cpp
```

(`std::print` needs `-lstdc++exp` on MinGW.) MSVC builds from
[`Ceres-ASM/Ceres-ASM.vcxproj`](../Ceres-ASM/Ceres-ASM.vcxproj) — only the **x64** configurations set
`stdcpp23` and the include directory, so build those. The test suite is a second executable,
[`Ceres-ASM/Ceres-ASM-Tests.vcxproj`](../Ceres-ASM/Ceres-ASM-Tests.vcxproj), or `sh tests/build.sh`
from the command line.

## Commands

| Command | What it does |
| --- | --- |
| `ceres asm <source.casm> [-o <output.cres>] [--listing] [--json] [--debug] [--emit-debug-json]` | Assembles a source file. Without `-o`, the source is only checked (parsed, translated, linked, emitted in memory) and discarded — useful as a pure syntax/semantics check. |
| `ceres run <file.casm\|file.cres> [--memory <bytes>]` | Runs a program, assembling it first if given a `.casm` source file. |
| `ceres disasm <file.casm\|file.cres> [--debug]` | Prints the `.text` section as address, encoded word, and disassembled instruction, one per line. |
| `ceres debug <file.casm\|file.cres> [<source2.casm> ...]` | Runs a program under the interactive debugger. See [The debugger](22-Debugger.md). |
| *(bare path)* | Shorthand for `run` — `ceres program.casm` is exactly `ceres run program.casm`. |

Global flags:

| Flag | Applies to | Effect |
| --- | --- | --- |
| `-o <path>` / `--output <path>` | `asm` | Write the assembled `.cres` to `<path>`. Without it, `asm` only validates and reports errors. |
| `--listing` | `asm`, `run` | Print an address/opcode/instruction listing of `.text` (via the disassembler) before running/after assembling. Gains a source-location column when combined with `--debug`. |
| `--json` | `asm` | Print diagnostics as a JSON array on stdout instead of human-readable text on stderr — meant for editor tooling to parse. See [Errors and diagnostics](17-Errors-and-Diagnostics.md). |
| `--debug` | `asm`, `run`, `disasm` | Build the line and symbol tables (see [Debug information](21-Debug-Information.md)). With `-o`, they are appended to the `.cres`. With `--listing`, each word is annotated with the source line it came from. On `disasm` of a `.cres`, reads back the tables the file already carries. |
| `--emit-debug-json` | `asm`, `disasm` | Print the debug tables as JSON on stdout. Implies `--debug`. |
| `--memory <bytes>` | `run`, `debug` | Overrides the VM's memory size (default 16 MiB — see [Memory](02-Memory.md)). |
| `--no-stop-on-entry` | `debug` | Start running immediately instead of stopping before the first instruction. |
| `-h` / `--help` | any | Prints usage and exits. |

## The assembly pipeline, stage by stage

`Assembler::assemble()` (in [`assembler.cpp`](../Ceres-ASM/src/assembler/assembler.cpp)) runs these
stages in order, stopping at the first one that reports an error:

```
1. Load + parse every source file (recursively following `import`)
        Lexer → Parser → std::vector<Statement>   (per file)
2. Build a TranslationUnit per file
        TranslationUnitBuilder: expand macros, resolve data types/literal values,
        assign addresses relative to the file's own sections, collect unresolved symbols
3. Link
        Linker: compute the shared memory map, relocate every unit's addresses into it,
        merge `global` symbols, resolve every remaining identifier operand
4. Emit
        BinaryEmitter: walk the fully-resolved AST and write out the final
        .text / .rodata / .data byte buffers, producing a vm::Program
        (and, when asked, the debug tables built from that same walk)
```

### 1. Parsing (per file)

`Assembler::loadTranslationUnit()` is the entry point for both the top-level source file(s) given on
the command line and every file reached transitively through `import` (see
[Modules and `import`](15-Modules-and-Import.md)). Each file is:

1. Read from disk (`readSourceFile`) and cached by its resolved path (`AssemblyState::cacheSourceFile`)
   so a file imported from multiple places is only read once.
2. Lexed and parsed (`Lexer` → `Parser` → `std::vector<Statement>`). At this stage, operands that are
   identifiers are **not yet resolved** to anything — the parser only recognizes their syntactic
   shape (register, immediate, memory operand, bare identifier, `$param`, `%%label`).
3. Marked "in progress" (`beginLoading`) for the duration of building its `TranslationUnit`, so an
   `import` cycle is detected instead of recursing forever (see
   [Modules and `import`](15-Modules-and-Import.md)).

### 2. Building a translation unit

`TranslationUnitBuilder::build()` walks every top-level `Statement` and, recursively, every statement
a macro call expands into (see [Macros](14-Macros.md)). For each statement it:

- Tracks the currently active section (`@text`/`@rodata`/`@data`/`@bss`) and the running byte offset
  within each of the four sections, **relative to the start of this file's own contribution** — not
  yet the final absolute address, since that depends on what other translation units precede this one
  in the linked image.
- Resolves data types and literal values for `let`/`const` declarations (including array-size
  constants), and records each variable/constant/label into this unit's own `SymbolTable`.
- For instructions, tries to resolve every operand *now* against this unit's own symbol table; an
  identifier that isn't found yet (because it's a `global` symbol defined in another file, or a
  file-level label defined later in the same file) is recorded as an `UnresolvedSymbol` for the
  linker to retry.
- Computes each instruction's byte size via `InstructionInfo::findMaxSizeInBytes()` — note this uses
  the **maximum** size across all overloads of a mnemonic (relevant for pseudo-instructions like
  `ldv`/`stv`, whose size doesn't actually vary by overload — see
  [Pseudo-instructions](06-Pseudo-Instructions.md)), since the exact overload can only be picked once
  operand types are fully known, but the size has to be reserved immediately in order to keep
  computing subsequent addresses.

The output is a `TranslationUnit`: a resolved-shape AST (`RelocatableStatement`s, each carrying the
address it will occupy *within its own section*), this file's own symbol table, its macro table, its
per-section byte sizes, and its list of still-unresolved symbols.

### 3. Linking

Covered in depth in [Labels and symbols → Linking](12-Labels-and-Symbols.md#linking-multiple-translation-units):
the linker computes the shared memory map (summing every unit's aligned section sizes), relocates
every unit's addresses into that shared image, merges `global` symbols, and does a final resolution
pass over every instruction's operands.

### 4. Emitting the binary

`BinaryEmitter::emit()` walks the now fully-resolved AST one more time and writes concrete bytes:

- A `.data`/`.rodata` declaration's literal value is serialized according to its data type (integers
  little-endian, floats as their raw IEEE-754 bit pattern, strings with their trailing null byte).
- An instruction statement is turned into its final encoded 32-bit word(s) via
  `InstructionInfo::find()` — the same lookup table introduced in
  [Instruction set](05-Instruction-Set.md) — using the operand types that are now fully known.
- Section buffers are padded to 4-byte alignment as needed (mirroring the linker's own section
  alignment).

The result is handed back as a `vm::Program`, ready either to be written to a `.cres` file
(`Program::saveToFile`) or run directly.

## Related pages

- [The `.cres` binary format](09-CRES-Binary-Format.md) — what `BinaryEmitter`'s output looks like on disk.
- [Errors and diagnostics](17-Errors-and-Diagnostics.md) — how each stage reports failures.
- [Labels and symbols](12-Labels-and-Symbols.md) — the linking pass in full detail.

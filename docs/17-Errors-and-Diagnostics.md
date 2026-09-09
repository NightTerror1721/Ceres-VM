# Errors and diagnostics

[← Back to index](README.md)

## How an error is represented internally

Every diagnostic the assembler produces is an `AssemblerErrorEntry`
(in [`errors.h`](../Ceres/libs/asm/include/ceres/asm/errors.h)):

```cpp
struct AssemblerErrorEntry
{
    std::string file;
    u32 line;
    u32 column;
    std::string message;
    DiagnosticSeverity severity;   // Error or Warning
};
```

`AssemblerErrorHandler` collects these into a `std::vector` as assembly proceeds — errors are
**accumulated, not thrown all the way out on the first one** wherever the code allows it (the parser
in particular recovers from a bad statement and keeps parsing the rest of the file, so a single
`ceres asm` invocation can report many independent mistakes at once instead of stopping at the
first). Internally, an `AssemblerError` (a `std::runtime_error` carrying a line/column) is the
*exception type* raised at the point a problem is detected; each stage's caller catches it and turns
it into an entry via `errorHandler.reportError(...)`.

Warnings live in the same list and are counted separately: `hasErrors()` asks whether anything
*failed*, `hasDiagnostics()` whether anything was said at all. A build with warnings and no errors
succeeds and writes its output.

## Warnings

The assembler has one warning today, and it exists because `global` created a category the language
did not have before: a declaration that provably **cannot** be reached from anywhere else. A private
`const`, `let` or `macro` that nothing in its own file names is dead with certainty.

```
$ ceres asm game.casm
  warning [game.casm:4] 'SLACK' is declared but never used, and is not global, so nothing
                        outside this file can use it either
```

Two things are deliberately exempt:

- **Anything `global`.** It is exported, so something outside this file may well be using it — the
  assembler cannot know, and saying otherwise would be wrong.
- **Struct field offsets and local labels.** Both are generated rather than written (see
  [Structs](23-Structs.md)), and warning about each unused field of a record would drown the
  warnings that matter.

Labels are not warned about either: a label nothing jumps to is often an entry point, a table marker
or a target reached through a register.

## Where each kind of error message comes from

The message text itself often names the stage that produced it, which is useful when debugging a
failure:

| Prefix / shape | Stage | Example |
| --- | --- | --- |
| *(none — a bare message)* | Lexer/Parser | `Unexpected token {lexeme}` |
| *(none — a bare message)* | Translation-unit build | `Data statement has a size of zero` |
| `Linker error: ...` | Linking | `Linker error: Unresolved symbol '{name}'.` |
| *(none — a bare message)* | Binary emission | (Rare — most problems are already caught earlier.) |

Every message also carries a **line number** (parsed statements always know their originating source
line) and, from the parser specifically, a **column**. Errors from later stages (linking, macro
tables, symbol tables) report `column = 1` for anything that isn't tied to a specific token, since
those stages work over already-parsed structures rather than raw source text.

## Human-readable output (default)

```
$ ceres asm broken.casm
Failed to assemble broken.casm
  [broken.casm:7] Unknown mnemonic or macro 'ad' taking 3 operand(s)
  [broken.casm:12] Linker error: Unresolved symbol 'undefined_label'.
  warning [broken.casm:2] 'SLACK' is declared but never used, and is not global, so nothing
                          outside this file can use it either
```

`main.cpp`'s `reportAssemblyErrors()` prints one line per diagnostic to **stderr**. A warning is
prefixed `warning`; the "Failed to assemble" header only appears when something actually failed, so a
build that only warns still succeeds and still says so.

## Machine-readable output (`--json`)

```bash
ceres asm broken.casm --json
```

```json
[{"file":"broken.casm","line":7,"column":1,"severity":"error","message":"Unknown mnemonic or macro 'ad' taking 3 operand(s)"},
 {"file":"broken.casm","line":2,"column":1,"severity":"warning","message":"'SLACK' is declared but never used, and is not global, so nothing outside this file can use it either"}]
```

Printed to **stdout** (not stderr), always as valid JSON — an empty array `[]` on success, so editor
tooling can parse the output unconditionally without special-casing the no-error case. Every field
is always present, including `severity`, which is `"error"` or `"warning"`. String values are escaped for the small fixed set of characters the assembler's own
messages can contain (quotes, backslashes, control characters) — see `jsonEscape()` in
[`main.cpp`](../Ceres/apps/cli/src/main.cpp); this is a minimal escaper for the shape of text the
assembler itself produces, not a general-purpose JSON serializer suitable for arbitrary input.

`--json` only changes how **assembly** diagnostics are reported (the `asm` command). It has no effect
on `run` or `disasm`, and it has no effect on runtime failures once a program is actually executing
(a VM fault like `AlignmentFault` is not an assembler diagnostic at all — see
[Interrupts and exceptions](08-Interrupts-and-Exceptions.md)).

## Checking a file without producing output

```bash
ceres asm program.casm
```

Running `asm` with no `-o` still runs the full pipeline (parse → build → link → emit, all the way
through), but simply discards the resulting bytes instead of writing them anywhere. This makes it a
complete, zero-side-effect way to validate a `.casm` file — exactly what a "check syntax" command in
an editor integration would want (this is in fact the entire reason `--json` exists: to let an
editor's language server run this and parse the result).

**With no `-o`, no entry point is required.** An entry point belongs to a *program*, not to a
translation unit: a library module has no `main` and is not wrong for it. Since the language server
runs this on every open document, one file at a time, demanding `main` here would put a diagnostic on
every module that is not the main one.

Everything else is still checked. The emitter still runs — it is only asked not to insist on an entry
point — so immediates that do not fit, branch targets out of range and unresolved symbolic offsets
are all still reported, which are exactly the errors worth having in an editor.

```bash
ceres asm lib/math.casm          # a library module: checks clean, no main needed
ceres asm lib/math.casm -o m.cres   # ERROR: Entry point label 'main' is not defined
```

## Common failure categories, and what stage they surface in

| Symptom | Likely stage | See also |
| --- | --- | --- |
| "Unexpected token", "Expected ... after ..." | Lexer/Parser | [Language syntax](10-Language-Syntax.md) |
| "Data statement has a size of zero", "Literal value does not match the specified data type" | Translation-unit build | [Data types and literals](11-Data-Types-and-Literals.md) |
| "Macro redefinition", "'$x' is not a parameter of macro" | Macro table / expansion | [Macros](14-Macros.md) |
| "Unresolved symbol", "Symbol '...' is defined in multiple translation units" | Linking | [Labels and symbols](12-Labels-and-Symbols.md) |
| "Invalid instruction syntax: MNEMONIC Reg Reg Imm" | Linking (operand resolved to an unsupported combination) | [Instruction set](05-Instruction-Set.md) |
| "Import cycle: '...' is already being assembled" | Module loading | [Modules and `import`](15-Modules-and-Import.md) |
| "'X' is declared in '...' but is not global, so it is not visible here" | Linking | [Modules and `import`](15-Modules-and-Import.md) |
| "'X' is exported by both '...' and '...'" | Operand resolution | [Modules and `import`](15-Modules-and-Import.md#when-two-modules-export-the-same-name) |
| "No import is named '...'", "'...' does not export '...'" | Operand resolution | [Modules and `import`](15-Modules-and-Import.md#named-imports) |
| "Cannot work out dimension N: this level has rows of A and of B elements" | Translation-unit build | [Data types and literals](11-Data-Types-and-Literals.md#multidimensional-arrays) |
| "A value of N does not fit in a 'irq': the largest is 63" | Translation-unit build | [Data types and literals](11-Data-Types-and-Literals.md#aliases-that-promise-a-range) |
| "Unsupported .cres version in file: ..." | Program loading | [The CRES binary format](09-CRES-Binary-Format.md#versioning) |

## Related pages

- [CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md) — the stages these diagnostics come from.
- [Known limitations](19-Known-Limitations.md) — gaps that show up as specific, expected error messages today.

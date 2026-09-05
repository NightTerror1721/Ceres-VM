# Errors and diagnostics

[← Back to index](README.md)

## How an error is represented internally

Every diagnostic the assembler produces is an `AssemblerErrorEntry`
(in [`errors.h`](../Ceres-ASM/src/assembler/errors.h)):

```cpp
struct AssemblerErrorEntry
{
    u32 line;
    u32 column;
    std::string message;
};
```

`AssemblerErrorHandler` collects these into a `std::vector` as assembly proceeds — errors are
**accumulated, not thrown all the way out on the first one** wherever the code allows it (the parser
in particular recovers from a bad statement and keeps parsing the rest of the file, so a single
`ceres asm` invocation can report many independent mistakes at once instead of stopping at the
first). Internally, an `AssemblerError` (a `std::runtime_error` carrying a line/column) is the
*exception type* raised at the point a problem is detected; each stage's caller catches it and turns
it into an entry via `errorHandler.reportError(...)`.

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
  [line 7] Unknown mnemonic or macro 'ad' taking 3 operand(s)
  [line 12] Linker error: Unresolved symbol 'undefined_label'.
```

`main.cpp`'s `reportAssemblyErrors()` prints one line per diagnostic to **stderr**, prefixed with the
file path that failed and, per entry, the line number.

## Machine-readable output (`--json`)

```bash
ceres asm broken.casm --json
```

```json
[{"line":7,"column":1,"severity":"error","message":"Unknown mnemonic or macro 'ad' taking 3 operand(s)"},
 {"line":12,"column":1,"severity":"error","message":"Linker error: Unresolved symbol 'undefined_label'."}]
```

Printed to **stdout** (not stderr), always as valid JSON — an empty array `[]` on success, so editor
tooling can parse the output unconditionally without special-casing the no-error case. Every field
is always present, including `severity`, which is currently always `"error"` (there's no warning
tier today). String values are escaped for the small fixed set of characters the assembler's own
messages can contain (quotes, backslashes, control characters) — see `jsonEscape()` in
[`main.cpp`](../Ceres-ASM/src/main.cpp); this is a minimal escaper for the shape of text the
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

## Common failure categories, and what stage they surface in

| Symptom | Likely stage | See also |
| --- | --- | --- |
| "Unexpected token", "Expected ... after ..." | Lexer/Parser | [Language syntax](10-Language-Syntax.md) |
| "Data statement has a size of zero", "Literal value does not match the specified data type" | Translation-unit build | [Data types and literals](11-Data-Types-and-Literals.md) |
| "Macro redefinition", "'$x' is not a parameter of macro" | Macro table / expansion | [Macros](14-Macros.md) |
| "Unresolved symbol", "Symbol '...' is defined in multiple translation units" | Linking | [Labels and symbols](12-Labels-and-Symbols.md) |
| "Invalid instruction syntax: MNEMONIC Reg Reg Imm" | Linking (operand resolved to an unsupported combination) | [Instruction set](05-Instruction-Set.md) |
| "Import cycle: '...' is already being assembled" | Module loading | [Modules and `import`](15-Modules-and-Import.md) |

## Related pages

- [CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md) — the stages these diagnostics come from.
- [Known limitations](19-Known-Limitations.md) — gaps that show up as specific, expected error messages today.

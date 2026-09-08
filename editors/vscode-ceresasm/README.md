# CeresASM for VSCode

Language support for **CASM**, the assembler of the [Ceres](../../README.md) virtual machine.

## Features (this version)

- Syntax highlighting for `.casm` files: sections, keywords, types, mnemonics, registers,
  labels, macro parameters (`$reg`) and hygienic macro labels (`%%loop`), literals and comments.
  Mnemonics and registers highlight case-insensitively (matching the real lexer); everything else
  is lowercase-only, also matching it.
- Live diagnostics: every time you edit or save a `.casm` file, the extension runs the real
  `ceres` compiler in the background (`ceres asm <file> --json`) and shows its errors as
  in-editor squiggles. The mark covers the whole statement rather than the single character
  the compiler pointed at - indentation and trailing comments excluded - because the column
  an assembler reports is where it noticed the problem, not where the problem is.
- Hover, on everything: mnemonics, pseudo-instructions, registers, keywords, directives
  (`align`, `org`, `assert`), types, section directives and the nine symbols the linker defines
  (`__heap_start`, `__text_end`, ...) show static documentation drawn from the top-level README.
  User symbols (`const`, `let`, labels, macros, `struct`s and their fields) show their declared
  value/type/visibility and, for macros, every arity in scope. A `struct` shows its whole layout
  with the byte offset each field name stands for; a field shows its own. Registers answer to
  their role as well as their number: `sp`, `fp` and `at` (and `lr`, the deprecated spelling of
  `at`).
- Go to definition: labels (including scope-correct resolution of repeated local `.name` labels
  across different subroutines), constants, variables, macros, structs and struct fields —
  resolved transitively through `import`, the same way the real assembler makes them visible, so
  a `call` into another file lands on the routine it names.
- Inlay hints, each one separately switchable in the settings: the declared type of a global next
  to a `ldv`/`stv` that names it, the value a `const` stands for, the byte offset behind a
  `Frame.field`, the register an `alias` names, the parameter names of a macro call, and the name
  of the device behind a port number in an `in`/`out` — with a note when that port is one of
  the ones reserved in the map but not backed by anything.
- Completion: mnemonics, pseudo-instructions, keywords, types, section directives, registers by
  number and by role, the linker-defined symbols, and every constant/variable/label/macro
  currently in scope.
- Find references and rename: labels (scope-correct for repeated local `.name` labels), constants,
  variables, macro parameters and hygienic macro labels, all within the files the language itself
  makes them visible in. Renaming a `.name` local label or a `$param`/`%%label` edits just the bare
  name, leaving the sigil untouched.
- Folding: each section (`@text`/`@rodata`/`@data`/`@bss`) up to the next section directive, each
  `macro`…`endmacro` block, and multi-line block comments.
- Semantic tokens: a second pass of highlighting on top of the syntax grammar that actually knows
  which identifiers are declared consts/variables/labels/macros/structs/fields (vs. plain
  undeclared text), using standard LSP token types so your theme's existing semantic colours
  apply.
- **Debugging**: press F5 on a `.casm` file. Breakpoints in the gutter, stepping by source line,
  registers and flags in the variables view, globals rendered through their declared types, the
  call stack, the disassembly view, and the hex memory viewer. Watchpoints break on reads as well
  as writes. See below.

None of this is fully scope-aware (e.g. completion doesn't filter out-of-scope local labels) —
this is a lightweight line-based index, not a re-parse of the language. See Known limitations
below for what rename/references deliberately don't reach.

## Requirements

You need a built `ceres` executable. From the repository root:

```sh
cd Ceres-ASM/src
g++ -std=c++23 -I. -o ceres.exe main.cpp vm/*.cpp assembler/*.cpp debug/*.cpp -lstdc++exp
```

The extension looks for it automatically at `Ceres-ASM/src/ceres[.exe]` under each open
workspace folder, then falls back to `ceres` on `PATH`. If neither exists, set the full path
explicitly:

```jsonc
// .vscode/settings.json
{
  "ceresAsm.compilerPath": "D:/Projects/CeresASM/Ceres-ASM/src/ceres.exe"
}
```

## Running it in development

This extension is not published yet. To try it:

1. `npm install` in this folder (installs both `client` and `server` workspaces).
2. `npm run compile`.
3. Open this folder in VSCode and press **F5** (Run Extension) to launch an Extension
   Development Host.
4. In that window, open a `.casm` file (e.g. `Ceres-ASM/examples/main.casm`).

Use the **CeresASM: Restart Language Server** command from the command palette if you change
`ceresAsm.compilerPath` and diagnostics don't update.

## Debugging

Press **F5** with a `.casm` file open. With no `launch.json` the extension fills one in for the
active file; otherwise the `casm` debug type takes:

```jsonc
{
  "type": "casm",
  "request": "launch",
  "name": "Debug the current CASM file",
  "program": "${file}",     // .casm source, or a .cres built with --debug
  "sources": [],            // extra .casm files to link in, as `ceres asm` would
  "stopOnEntry": true,
  "memory": 16777216,       // machine memory in bytes
  "ceresPath": "",          // empty uses ceresAsm.compilerPath, then autodetection
  "trace": false            // print the debugger command line to the Debug Console
}
```

What works:

| | |
| --- | --- |
| Breakpoints | Click the gutter. Also function breakpoints (by label) and instruction breakpoints in the disassembly view. |
| Stepping | Step over / into / out by **source line**, plus instruction-level stepping from the disassembly view. |
| Variables | Registers, flags, float registers and globals. Globals are rendered through their declared types — a `u8[]` shows as a quoted string. |
| Watch and hover | Full expressions: `r3`, `counter`, `scores[2]`, `[r1 + 4]`, `u8[r2]`, `sp < 0x1000`. |
| Conditional breakpoints | Right-click a breakpoint for a condition, a hit count, or a log message. |
| Watchpoints | *Break on Value Change* on any global in the variables view. |
| Exceptions | The Breakpoints pane lists the machine's six faults; uncheck one to let it through. |
| Call stack | Reconstructed by watching `CALL`/`RET` go past; interrupt handlers appear as their own frames. |
| Memory | The hex editor's *View Binary Data* on any variable or register. |
| Disassembly | *Open Disassembly View*, annotated with the source line each word came from. |
| Editing state | Set a register from the variables view; the program counter too. |
| Running backwards | Step Back and Reverse Continue, because the machine is deterministic enough for both to be exact rather than approximate. |

### Feeding the program input

The debugger owns the Debug Console, so a program reading from the terminal port has no keyboard
of its own. Type `>` followed by the text into the Debug Console, or run **CeresASM: Send Input to
the Running Program** from the command palette. Both queue a line into the terminal device — which
is what the tutorial's two games need.

### How it is wired

The extension does not use `@vscode/debugadapter`. VSCode's inline adapter API hands over
already-parsed DAP messages, so the library's job — Content-Length framing and a base class — is
not needed, and the extension keeps a dependency list as short as the rest of the project's.
`client/src/debugAdapter.ts` translates DAP to the protocol that `ceres debug --server` speaks,
which is deliberately a different, smaller vocabulary in the machine's own terms.

## Known limitations

- Diagnostics are computed against a temporary sibling copy of the file being edited (so unsaved
  changes are checked too), not the file on disk. It's written next to the original so relative
  `import "..."` paths still resolve.
- The compiler's diagnostic entries don't carry a file name, so errors inside an `import`ed file
  are reported at that file's own line/column but attributed to the document you're editing.
- Debugging a `.cres` built without `--debug` works, but only in addresses: breakpoints by line
  come back unverified and there are no source lines. Debug the `.casm` source directly, or
  assemble with `ceres asm --debug`.
- The call stack is walked through the frame pointers where the assembler recorded that the
  function opens one with `enter`, and those frames are exact. A function that opens no frame has
  no chain to walk, and the stack falls back to being inferred from the calls gone past — those
  frames are greyed in the call stack view, and code that unwinds by hand can desynchronise them.
- Inlay hints are drawn from the same line-based index as everything else, so a struct field's
  offset is shown only where the layout could be worked out here: an array sized by an expression
  the index cannot fold leaves that field, and every one after it, without an offset rather than
  with a guessed one.
- Find references/rename for a `const` or `macro` only reach the current file plus whatever it
  transitively `import`s — not other, unrelated files elsewhere in the workspace that happen to
  import the same one. Renaming a shared constant or macro will warn you when it touched more than
  one file, but it cannot discover importers it hasn't been asked to look at; check the rest of
  the workspace by hand afterwards.

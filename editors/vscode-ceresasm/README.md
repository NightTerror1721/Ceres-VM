# CeresASM for VSCode

Language support for **CASM**, the assembler of the [Ceres](../../README.md) virtual machine.

## Features (this version)

- Syntax highlighting for `.casm` files: sections, keywords, types, mnemonics, registers,
  labels, macro parameters (`$reg`) and hygienic macro labels (`%%loop`), literals and comments.
  Mnemonics and registers highlight case-insensitively (matching the real lexer); everything else
  is lowercase-only, also matching it.
- Live diagnostics: every time you edit or save a `.casm` file, the extension runs the real
  `ceres` compiler in the background (`ceres asm <file> --json`) and shows its errors as
  in-editor squiggles, at the exact line and column the compiler reports.
- Hover: mnemonics, pseudo-instructions, registers, keywords, types and section directives show
  static documentation drawn from the top-level README. User symbols (`const`, `let`, labels,
  macros) show their declared value/type/visibility and, for macros, every arity in scope.
- Go to definition: labels (including scope-correct resolution of repeated local `.name` labels
  across different subroutines), constants, variables and macros — the last two resolved
  transitively through `import`, the same way the real assembler makes them visible.
- Completion: mnemonics, pseudo-instructions, keywords, types, section directives, registers,
  and every constant/variable/label/macro currently in scope.
- Find references and rename: labels (scope-correct for repeated local `.name` labels), constants,
  variables, macro parameters and hygienic macro labels, all within the files the language itself
  makes them visible in. Renaming a `.name` local label or a `$param`/`%%label` edits just the bare
  name, leaving the sigil untouched.
- Folding: each section (`@text`/`@rodata`/`@data`/`@bss`) up to the next section directive, each
  `macro`…`endmacro` block, and multi-line block comments.
- Semantic tokens: a second pass of highlighting on top of the syntax grammar that actually knows
  which identifiers are declared consts/variables/labels/macros (vs. plain undeclared text), using
  standard LSP token types so your theme's existing semantic colours apply.
- **Debugging**: press F5 on a `.casm` file. Breakpoints in the gutter, stepping by source line,
  registers and flags in the variables view, globals rendered through their declared types, a
  reconstructed call stack, the disassembly view, and the hex memory viewer. See below.

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
- The call stack is reconstructed rather than unwound, because `CALL` pushes only a return address
  and no register tracks frames. Code that unwinds by hand can desynchronise it; the disassembly
  view is the ground truth.
- A watchpoint only detects writes, and detects them by comparing the watched bytes between
  instructions rather than by trapping the access: a read is invisible to it, and so is a write
  that puts back the value that was already there.
- Find references/rename for a `const` or `macro` only reach the current file plus whatever it
  transitively `import`s — not other, unrelated files elsewhere in the workspace that happen to
  import the same one. Renaming a shared constant or macro will warn you when it touched more than
  one file, but it cannot discover importers it hasn't been asked to look at; check the rest of
  the workspace by hand afterwards.

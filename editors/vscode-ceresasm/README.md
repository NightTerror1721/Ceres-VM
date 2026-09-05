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

None of this is fully scope-aware (e.g. completion doesn't filter out-of-scope local labels) —
this is a lightweight line-based index, not a re-parse of the language. See Known limitations
below for what rename/references deliberately don't reach.

## Requirements

You need a built `ceres` executable. From the repository root:

```sh
cd Ceres-ASM/src
g++ -std=c++23 -I. -o ceres.exe main.cpp vm/*.cpp assembler/*.cpp -lstdc++exp
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

## Known limitations

- Diagnostics are computed against a temporary sibling copy of the file being edited (so unsaved
  changes are checked too), not the file on disk. It's written next to the original so relative
  `import "..."` paths still resolve.
- The compiler's diagnostic entries don't carry a file name, so errors inside an `import`ed file
  are reported at that file's own line/column but attributed to the document you're editing.
- Find references/rename for a `const` or `macro` only reach the current file plus whatever it
  transitively `import`s — not other, unrelated files elsewhere in the workspace that happen to
  import the same one. Renaming a shared constant or macro will warn you when it touched more than
  one file, but it cannot discover importers it hasn't been asked to look at; check the rest of
  the workspace by hand afterwards.

# Ceres Wiki

Ceres is a 32-bit virtual machine with its own instruction set, plus an assembler
(`CeresASM`, `.casm` files) that compiles to that instruction set. The whole project is written in
C++23 and lives under [`Ceres-ASM/src`](../Ceres-ASM/src).

This wiki documents the assembler and the virtual machine in enough detail to write `.casm`
programs from scratch, understand exactly how every instruction is encoded in binary, and know what
the assembler does internally at each stage of the build.

## Table of contents

### The virtual machine

1. [Architecture overview](01-Overview.md) — what's in the repository and how the pieces fit together.
2. [Memory](02-Memory.md) — memory map, protected segments, the stack, alignment.
3. [Registers and flags](03-Registers-and-Flags.md) — the integer and floating-point register banks, the flags register.
4. [Instruction format](04-Instruction-Format.md) — the 32-bit encoding, and how fields overlap.
5. [Instruction set](05-Instruction-Set.md) — full reference for every real instruction, grouped by category.
6. [Pseudo-instructions](06-Pseudo-Instructions.md) — `la`, `ldv`, `stv`, `neg`, `ifXX`, and how they expand.
7. [I/O devices and ports](07-IO-Devices-and-Ports.md) — the port map, the terminal, the timer, system control.
8. [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — the vector table, `INT`/`IRET`, hardware faults.
9. [The `.cres` binary format](09-CRES-Binary-Format.md) — the executable header and the loaded program's memory layout.

### The assembly language (CASM)

10. [Language syntax](10-Language-Syntax.md) — lexical rules, comments, sections, addressing.
11. [Data types and literals](11-Data-Types-and-Literals.md) — scalars, arrays, strings, natural alignment.
12. [Labels and symbols](12-Labels-and-Symbols.md) — global/file/local levels, resolution and linking.
13. [Constants and constant expressions](13-Constants-and-Expressions.md) — `const`, compile-time arithmetic.
14. [Macros](14-Macros.md) — parameters, arity-based overloading, hygienic labels, recursion.
15. [Modules and `import`](15-Modules-and-Import.md) — how multiple source files are combined.

### Tooling and workflow

16. [CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md) — the `asm`/`run`/`disasm` commands, the internal pipeline.
17. [Errors and diagnostics](17-Errors-and-Diagnostics.md) — the message format, warnings, `--json` output.
18. [Annotated examples](18-Annotated-Examples.md) — a line-by-line walkthrough of a full program.
19. [Known limitations](19-Known-Limitations.md) — what the project doesn't do yet.
21. [Debug information](21-Debug-Information.md) — the line and symbol tables, `--debug`, and how they ride along in a `.cres`.
22. [The debugger](22-Debugger.md) — `ceres debug`: breakpoints, stepping by source line, the reconstructed call stack.

### Learn by doing

20. [Tutorial práctico (Spanish)](20-Tutorial-Practico.md) — progressive exercises building up to
    two playable terminal games (rock-paper-scissors and tic-tac-toe).

## What changed recently

If you already knew this language, these are the parts that moved:

- **Visibility.** `global` now applies to `const`, `let`, `macro` and `struct`, not just labels, and
  nothing without it leaves its file — [Labels and symbols](12-Labels-and-Symbols.md).
- **Imports** are references rather than copies, so a module is merged once however many routes
  reach it, and `import ... as name` disambiguates a clash — [Modules and `import`](15-Modules-and-Import.md).
- **Comparison jumps** (`jgr`/`jge`/`jls`/`jle` signed, `jab`/`jae`/`jbl`/`jbe` unsigned) and the
  `ifXX` family. Adding them **renumbered every opcode above the control-flow block**, so a `.cres`
  built before this is rejected rather than misread — [Instruction set](05-Instruction-Set.md), [The `.cres` binary format](09-CRES-Binary-Format.md#versioning).
- **Constant expressions** may name other constants, use parentheses, and ask `sizeof`/`countof`/`dimof` — [Constants and expressions](13-Constants-and-Expressions.md).
- **Multidimensional arrays**, with any dimension inferable from the initializer — [Data types and literals](11-Data-Types-and-Literals.md#multidimensional-arrays).
- **New type aliases**: `ptr`, `port`, `irq`, `byte`, `half`, `word` — [Data types and literals](11-Data-Types-and-Literals.md#aliases).
- **Register aliases** (`alias cursor = r5`) and **structs** — [Language syntax](10-Language-Syntax.md#register-aliases), [Structs](23-Structs.md).
- **Warnings**, starting with unused private declarations — [Errors and diagnostics](17-Errors-and-Diagnostics.md#warnings).
- **Memory displacements are signed.** `[fp - 8]` now reaches below the base instead of 65528 bytes
  above it, which is what makes a frame pointer usable — and a second reason a `.cres` built before
  this is rejected — [Instruction format](04-Instruction-Format.md#signed-and-unsigned-immediate-fields).
- **A calling convention** you can actually import — [A calling convention](24-Calling-Convention.md).

## Quick start

```bash
ceres asm examples/main.casm -o hello.cres
ceres run hello.cres
Hello, CeresVM!
```

A `.casm` file is compiled with `ceres asm`, producing a `.cres` binary that the VM (`ceres run`)
can load and execute directly. You can also run a `.casm` file without producing a `.cres` first:
`ceres run` assembles it in memory before booting it.

## How to use this wiki

Each page stands on its own but links to the related ones. If this is your first time approaching
the project, follow the table of contents in order: first the machine (what hardware it simulates),
then the language (how to write code targeting that machine), then the tooling.

Every code example uses the real `.casm` syntax as accepted by the parser in
[`Ceres-ASM/src/assembler`](../Ceres-ASM/src/assembler); wherever a behaviour has a non-obvious
reason behind it (for example, why `str` takes the base register before the value), the *why* is
explained, not just the *what*.

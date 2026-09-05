# Architecture overview

[← Back to index](README.md)

Ceres has two halves living in the same binary (`ceres` / `ceres.exe`): an **assembler**
(`ceres::casm`, in [`src/assembler`](../Ceres-ASM/src/assembler)) and a **virtual machine**
(`ceres::vm`, in [`src/vm`](../Ceres-ASM/src/vm)). The entry point is
[`src/main.cpp`](../Ceres-ASM/src/main.cpp), which decides from the command-line arguments whether
to assemble, run, or disassemble.

## The two halves

```
   .casm source                assembler (ceres::casm)              .cres binary
┌───────────────────┐       ┌──────────────────────────┐      ┌──────────────────┐
│ text                │  →   │ Lexer → Parser → Translation│  →  │ ProgramHeader     │
│ (one or more files  │       │ Unit → Linker → Binary     │      │ + .text/.rodata/  │
│  tied together by   │       │ Emitter                     │      │   .data           │
│  `import`)          │       └──────────────────────────┘      └──────────────────┘
└───────────────────┘                                                    │
                                                                          ▼
                                                          ┌──────────────────────────┐
                                                          │ Virtual machine (ceres::vm)│
                                                          │ Memory + Registers +       │
                                                          │ ExecutionEngine + IOPorts  │
                                                          └──────────────────────────┘
```

- The **assembler** never executes code: it only ever produces bytes. See
  [CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md) for the exact stages
  (lexer → parser → `TranslationUnit` → `Linker` → `BinaryEmitter`).
- The **VM** never knows anything about `.casm`: it only understands already-encoded 32-bit
  instructions. It can load either a `.cres` file saved to disk or the `Program` the assembler just
  produced in memory (that's what `ceres run program.casm` does).

## Map of the source tree

| Folder | Contents |
| --- | --- |
| [`src/assembler`](../Ceres-ASM/src/assembler) | Lexer, parser, symbol table, macro table, linker, binary emitter. |
| [`src/vm`](../Ceres-ASM/src/vm) | Memory, registers, execution engine, I/O devices, interrupt controller, disassembler. |
| [`src/common`](../Ceres-ASM/src/common) | Shared utilities: integer types (`u8`…`u32`, `i8`…`i32`, `int24`), assertions, a fixed-size vector. |
| [`tests`](../tests) | The test suite (108 cases, 335 assertions), which documents expected behaviour with runnable examples. |
| [`editors/vscode-ceresasm`](../editors/vscode-ceresasm) | VS Code extension (syntax highlighting + language server) for `.casm`. |

## Key assembler files and what they solve

| File | Responsibility |
| --- | --- |
| [`lexer.h`/`.cpp`](../Ceres-ASM/src/assembler/lexer.cpp) | Turns text into `Token`s: identifiers, keywords, literals, punctuation. |
| [`parser.h`/`.cpp`](../Ceres-ASM/src/assembler/parser.cpp) | Turns tokens into `Statement`s (sections, labels, data, instructions, macros, imports). Nothing is resolved yet: operands that are identifiers stay as identifiers. |
| [`translation_unit.h`/`.cpp`](../Ceres-ASM/src/assembler/translation_unit.cpp) | Walks a file's `Statement`s, expands macro calls, computes addresses relative to the file, resolves data types and literal values. |
| [`macro_table.h`/`.cpp`](../Ceres-ASM/src/assembler/macro_table.cpp) | Stores defined (or imported) macros, indexed by name **and** parameter count. |
| [`symbol_table.h`/`.cpp`](../Ceres-ASM/src/assembler/symbol_table.cpp) | The symbol table (labels, constants, variables) of one translation unit, or the global one after linking. |
| [`linker.h`/`.cpp`](../Ceres-ASM/src/assembler/linker.cpp) | Combines translation units: computes the final memory map, relocates addresses, resolves symbols across files. |
| [`binary_emitter.h`/`.cpp`](../Ceres-ASM/src/assembler/binary_emitter.cpp) | Turns the already-resolved AST into the final `.text`, `.rodata` and `.data` bytes, producing a `vm::Program`. |
| [`instruction_info.h`/`.cpp`](../Ceres-ASM/src/assembler/instruction_info.cpp) | The master table: for every combination of mnemonic + operand types, says which VM opcode(s) to emit. This is where the assembler "picks the encoding". |
| [`assembler.h`/`.cpp`](../Ceres-ASM/src/assembler/assembler.cpp) | Orchestrates all of the above: `Assembler::assemble()` is the public entry point. |

## Key VM files

| File | Responsibility |
| --- | --- |
| [`memory.h`](../Ceres-ASM/src/vm/memory.h) | The flat byte array, with bounds-checked and unchecked reads/writes. |
| [`registers.h`](../Ceres-ASM/src/vm/registers.h) | `Register`, the 16-entry general-purpose register bank, and `FlagRegister`. |
| [`fregisters.h`](../Ceres-ASM/src/vm/fregisters.h) | The 16-entry floating-point register bank. |
| [`instructions.h`](../Ceres-ASM/src/vm/instructions.h) | Encoding/decoding of a 32-bit `Instruction`: extracting `opcode`, `rd`, `rs`, `rt`, `imm8/16/24`. |
| [`opcodes.h`](../Ceres-ASM/src/vm/opcodes.h) | The `Opcode` enum: every real operation code the VM knows how to execute. |
| [`execution_engine.h`/`.cpp`](../Ceres-ASM/src/vm/execution_engine.cpp) | The interpreter: `step()` fetches, decodes and executes one instruction; a per-opcode function-pointer table does the dispatch. |
| [`interrupt_controller.h`](../Ceres-ASM/src/vm/interrupt_controller.h) | The queue of pending interrupts that devices use to signal the CPU. |
| [`io_ports.h`](../Ceres-ASM/src/vm/io_ports.h) | The 256 I/O lines and the mechanism for attaching `IODevice`s to them. |
| [`devices.h`](../Ceres-ASM/src/vm/devices.h) | The devices that exist today: `SystemControlDevice`, `TimerDevice`, `TerminalDevice`. |
| [`bios.h`](../Ceres-ASM/src/vm/bios.h) | The minimal BIOS: writes the vector table and a three-instruction stub into the protected segment. |
| [`program.h`/`.cpp`](../Ceres-ASM/src/vm/program.cpp) | The `Program` container (header + sections) and its serialization to/from `.cres`. |
| [`ceresvm.h`/`.cpp`](../Ceres-ASM/src/vm/ceresvm.cpp) | The high-level `CeresVM` class that ties together memory, execution engine, ports and interrupts, and knows how to load a `Program`. |
| [`disassembler.h`](../Ceres-ASM/src/vm/disassembler.h) | Turns encoded instructions back into readable text (used by `--listing` and `ceres disasm`). |

## Design philosophy worth knowing up front

- **The assembler picks the encoding, not the programmer.** For example, `add r1, r2, r3` and
  `add r1, r2, 5` are the same mnemonic (`ADD`), but they compile to different opcodes
  (`ADD` vs. `ADDI`) depending on the operand type. This is covered in detail in
  [Instruction set](05-Instruction-Set.md).
- **Floating-point registers reuse the exact same bit fields as integer ones.** `add f1, f2, f3`
  assembles to `FADD` using exactly the same bit positions that `rd/rs/rt` would use for integer
  registers. There is no separate encoding space for floats.
- **Nothing is resolved in a single pass.** The parser doesn't know anything about a valid register
  beyond recognizing its name; the translation unit doesn't know final addresses until the linker
  fixes the memory map; the binary emitter doesn't decide anything, it only writes out bytes that
  were already decided. This layering is why error messages clearly say which stage they came from
  (for example, "Linker error: ...").

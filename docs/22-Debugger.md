# The debugger

[← Back to index](README.md)

```
ceres debug examples/main.casm
```

Runs a program under an interactive debugger: breakpoints by source line, label or address, four
kinds of stepping, registers, memory, global variables, and a reconstructed call stack. Until this
existed the only debugging tool the project had was a static listing.

## What it is built on

Two things made it small rather than large. The machine already stepped one instruction at a time —
`ExecutionEngine::step()` has always been the primitive a debugger needs — and
[the line and symbol tables](21-Debug-Information.md) already say where every instruction came from.
`DebugSession` (in [`debug_session.h`](../Ceres-ASM/src/debug/debug_session.h)) is what sits between
them: it owns the machine and its devices and drives the step loop itself instead of handing it to
`CeresVM::run()`, which is what lets it decide between one instruction and the next.

It answers in plain structs and knows nothing about terminals or protocols, so
[`tests/test_debugger.cpp`](../tests/test_debugger.cpp) drives it exactly the way a real debugger
does. `DebugCLI` is one front end over it; the editor integration is another.

## Commands

| | |
| --- | --- |
| `c` | Continue until a breakpoint, a fault, or the end |
| `s` | Step one source line, entering calls |
| `n` | Step one source line, running calls to completion |
| `si [count]` | Step one machine instruction |
| `fin` | Run until the current subroutine returns |
| `until <loc>` | Run until an address, symbol or `file:line` is reached |
| `run` | Restart from the beginning |
| `b <loc>` | Break at `file:line`, a label, a bare line number, or `*0x400` |
| `d [id]` | Delete one breakpoint, or all of them |
| `bl` | List breakpoints and their hit counts |
| `regs` | Registers and flags |
| `bt` | Call stack |
| `l [line]` | Source around the current line |
| `dis [count]` | Disassembly around the program counter |
| `x <loc> [n]` | Hex dump; `x msg` dumps exactly that variable |
| `vars` | Globals and constants, rendered through their declared types |
| `p <name>` | One variable or constant |
| `set <reg> <value>` | Write a register |
| `input <text>` | Feed a line to the program's terminal input |
| `q` | Quit |

## A session

```
$ ceres debug examples/main.casm
Ceres debugger. 'h' for the command list, 'q' to quit.
entry at main.casm:10 (0x00000400)
      8 | @text
      9 | global main:
->   10 |     la r1, HELLO_MSG
     11 |     call print
     12 |     li r0, EXIT_CODE
(ceres) b print
  Breakpoint 1 at 0x00000434 (main.casm:28)
(ceres) c
Breakpoint 1, breakpoint at main.casm:28 (0x00000434)
     27 | print:
->   28 |     mov r3, r1  // Save the original pointer
     29 |     call strlen
(ceres) bt
  #0 print                        main.casm:28
  #1 main                         main.casm:11
  (reconstructed: CALL only pushes a return address, so frames are inferred)
(ceres) vars
  EXIT_CODE            const      u32          = 1
  HELLO_MSG            0x00000468 u8[16]       = "Hello, CeresVM!"
```

## Stepping, and why one line is not one instruction

`s` and `n` step by **source line**, which in this language is rarely one instruction:

- `la` is two words and `ldv`/`stv` are three (see
  [Pseudo-instructions](06-Pseudo-Instructions.md)), so `stv r0, total` is one step that retires
  three instructions.
- A macro call expands to as many instructions as its body has, all belonging to the line that
  wrote the call.

The debugger stops only at addresses flagged `FirstOfLine` in the line table, which is what keeps
one source line from looking like three steps. `si` is there for when you want the machine's own
granularity.

`n` differs from `s` only in what it does with a `CALL`: it keeps going until the shadow call stack
is back at the depth it started from.

## The call stack is reconstructed, and says so

`CALL` pushes a return address and nothing else. `fp` is defined but no instruction touches it, and
there is no calling convention (see [Known limitations](19-Known-Limitations.md)). So the stack
cannot be unwound after the fact — it has to be *built*, by watching each instruction go past:

- `CALL`/`CALLR` pushes a frame, unless the push overflowed the stack, in which case nothing jumped.
- `RET`/`IRET` pops one.
- An interrupt being taken pushes a frame marked as a handler, with the faulting address recorded
  as where it will return to.
- Anything that moves the stack pointer above a frame's own entry value pops that frame, which
  catches code that unwinds by hand instead of with `RET`.

The outermost frame is never popped: a `RET` without its `CALL` means the program is doing something
the shadow stack cannot follow, and an empty stack would be worse than a stale one. `bt` prints the
caveat every time rather than pretending otherwise, and the disassembly view is always available as
the ground truth.

## Faults

A fault reports **where it happened**, not where it went:

```
(ceres) c
AlignmentFault raised at fault.casm:4
-> 00000100  42000045  LI r0, 69
(ceres) bt
  #0 AlignmentFault handler       0x00000100  (interrupt)
  #1 main                         fault.casm:4
```

By the time the debugger regains control the program counter is already inside the BIOS handler, so
the address of the faulting instruction is the one thing that cannot be recovered afterwards.
`ExecutionEngine` hands it over through an interrupt observer, captured before the dispatch
redirects anything. Without that, every fault would look like it happened in the BIOS.

## Nothing runs away

Every run mode is bounded. `c` gives up after 200 million instructions and `s`/`n` after ten
million, reporting a step limit rather than hanging:

```
(ceres) c
Still running after 200000000 instructions - stopped so you can look. 'c' to carry on.
```

A machine sitting in `halt` with no armed timer and no pending interrupt is reported straight away
rather than spinning out the budget a millisecond at a time — that is a program that can never wake,
and it is worth being told so.

## Input

The debugger reads its own commands from stdin, so a program's input cannot simply be typed
through: the two would compete for the same keystrokes. `input <text>` queues a line into the
terminal device's ring buffer instead.

```
(ceres) input hola
  Queued 5 byte(s) of input.
```

## Debugging without debug information

`ceres debug program.cres` on a file built without `--debug` still works, just blind: addresses,
registers, memory and disassembly, but no source lines. `s` degrades to stepping one instruction,
and a line breakpoint says why it cannot be set rather than silently doing nothing.

## Related pages

- [Debug information](21-Debug-Information.md) — the line and symbol tables everything here reads.
- [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — the faults reported above.
- [Pseudo-instructions](06-Pseudo-Instructions.md) — why one line is several addresses.
- [Known limitations](19-Known-Limitations.md) — including what the debugger still does not do.

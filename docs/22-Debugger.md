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
| `b <loc> if <expr>` | Break only when the expression is true |
| `log <loc> <text>` | Log `{expressions}` and carry on, instead of stopping |
| `watch <name>` | Stop when that memory changes |
| `d [id]` | Delete one breakpoint or watch, or all of them |
| `bl` | List breakpoints and their hit counts |
| `regs` | Registers and flags |
| `bt` | Call stack |
| `l [line]` | Source around the current line |
| `dis [count]` | Disassembly around the program counter |
| `x <loc> [n]` | Hex dump; `x msg` dumps exactly that variable |
| `vars` | Globals and constants, rendered through their declared types |
| `p <expr>` | Evaluate an expression |
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

## Expressions

Watches, hover, breakpoint conditions and logpoint messages all go through one small evaluator
([`expression.h`](../Ceres-ASM/src/debug/expression.h)). Nothing in the assembler could be reused
for it: constant folding runs at assembly time and cannot even reference an identifier.

| | |
| --- | --- |
| `r3` `f1` `sp` `fp` `lr` `pc` `ticks` | Registers and machine state |
| `zero` `sign` `carry` `overflow` `interrupt` `halting` `trap` | Flags, as 0 or 1 |
| `1024` `0x400` `0b1010` `3.5` | Literals |
| `LIMIT` | A constant, with the value the assembler recorded |
| `counter` | A variable, read from live memory through its declared type |
| `scores[2]` | One element, indexed by the element type rather than by bytes |
| `[r1 + 4]` | A word loaded from memory |
| `u8[r2]` `i16[sp + 8]` | A typed load |
| `main` | A label, as its address |
| `+ - * / % << >> & \| ^ ~ ! && \|\| == != < <= > >=` | The arithmetic to combine them |

```
(ceres) p r3 * 2 + 1
  r3 * 2 + 1 : i64 = 11
(ceres) p scores[1]
  scores[1] : i16 = -20
(ceres) p counter
  counter : u32 = 5   at 0x00000428
```

Signedness comes from the type: `scores` is `i16[3]`, so `scores[1]` reads as `-20` rather than as
`65516`. A `u8[]` renders as the string it almost always is.

## Conditional breakpoints, hit counts and logpoints

```
(ceres) b 42 if r3 == 5
(ceres) log 42 counter is {counter}, r3={r3}
```

The three compose in one order, and it is the order that makes them useful:

1. **The condition** decides whether this arrival is a hit at all. An arrival that fails it is not
   counted, so a hit count means "how many times did this actually trigger".
2. **The hit count** (`5`, `>5`, `>=5`, `==5`, `%3`) decides whether this hit stops anything.
3. **The log message**, if there is one, is reported and the program carries on. That is the whole
   of what makes a logpoint not a breakpoint.

A condition that cannot be evaluated **stops the machine and says why**. Silently never firing
would leave you watching a breakpoint with no clue as to the reason.

## Watchpoints

```
(ceres) watch counter
  Watch 1 on 4 bytes at 0x00000428
(ceres) c
Watch 1, counter changed, data breakpoint at loop.casm:9 (0x00000414)
```

A named variable knows its own extent, so `watch counter` watches exactly it. Only **writes** are
detected, and by comparing the bytes to a snapshot between instructions rather than by trapping the
access: the machine has no memory hook, and adding one would put a branch in the hot path of every
load and store for the sake of something almost no run uses. The cost is proportional to the bytes
actually being watched.

## Choosing which faults stop the machine

By default every one of the machine's system exceptions stops it. A client can narrow that — in
VSCode, through the Breakpoints pane's exception checkboxes — and a fault that is not selected goes
to its handler unremarked, exactly as it would with no debugger attached.

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

## In VSCode

The extension in [`editors/vscode-ceresasm`](../editors/vscode-ceresasm/README.md) debugs `.casm`
files with **F5**: breakpoints in the gutter, stepping by source line, registers and flags in the
variables view, globals rendered through their declared types, the reconstructed call stack, the
disassembly view and the hex memory editor.

It works through a third face of the same session:

```
ceres debug program.casm --server
```

which speaks newline-delimited JSON on stdin and stdout instead of running a REPL. That protocol is
deliberately **not** the Debug Adapter Protocol — it is a small vocabulary in the machine's own
terms (addresses, registers, ports, ticks, sections), and the extension translates. Two reasons:
DAP's awkward corners stay on the side where a library already handles them, and the protocol stays
something a script can drive without implementing DAP at all.

```
→ {"seq":1,"type":"request","command":"setFunctionBreakpoints","arguments":{"names":["print"]}}
← {"type":"response","request_seq":1,"command":"setFunctionBreakpoints","success":true,"body":{...}}
→ {"seq":2,"type":"request","command":"configurationDone"}
← {"type":"event","event":"stopped","body":{"reason":"entry","file":"main.casm","line":10,...}}
```

Requests are read on their own thread so `pause` can be acted on while the machine is running;
everything else is queued and handled in order. The program's own output travels as `output`
events rather than being written to stdout, which it would otherwise corrupt.

### Bytes, not text

The terminal device emits **bytes**: a multi-byte UTF-8 character reaches it as several separate
port writes. They cross the protocol as hex and are decoded on the editor side with a streaming
decoder, because the adapter is the first place that can safely know where a character ends.
Decoding each write on its own would turn every accented letter in the Spanish tutorial into two
replacement characters.

### Feeding a program its input

The debugger owns the Debug Console, so a program reading port `0x02` has no keyboard of its own.
Typing `>` followed by text in the Debug Console sends that line to the program, as does the
**CeresASM: Send Input to the Running Program** command.

## Related pages

- [Debug information](21-Debug-Information.md) — the line and symbol tables everything here reads.
- [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — the faults reported above.
- [Pseudo-instructions](06-Pseudo-Instructions.md) — why one line is several addresses.
- [Known limitations](19-Known-Limitations.md) — including what the debugger still does not do.
- [The VSCode extension](../editors/vscode-ceresasm/README.md) — the editor side of all this.

# Known limitations

[← Back to index](README.md)

The assembler and the VM work end to end — every example in this wiki assembles and runs — but a
number of things are explicitly not done yet. This page collects them in one place so they aren't
mistaken for bugs.

## No enforced calling convention

`fp` (`r14`) is defined and named, but **no instruction in the VM reads or writes it specially** — it
behaves exactly like any other general-purpose register (see
[Registers and flags](03-Registers-and-Flags.md)). No register is documented as caller-saved or
callee-saved. The closest thing to an actual convention in the whole project is that `r12` gets
silently clobbered by three pseudo-instructions (`ldv`, `stv`, and the float form of `la` where a
scratch register is needed — see [Pseudo-instructions](06-Pseudo-Instructions.md)). Any argument-
passing or register-preservation convention beyond that (as used informally in
[Annotated examples](18-Annotated-Examples.md)) is purely a habit the programmer has to maintain by
hand — nothing enforces it.

Since nothing in the VM enforces a convention, the practical way to get one is to write it as a pair
of macros and use them consistently — see
[Macros → Worked example: a calling convention built out of macros](14-Macros.md#worked-example-a-calling-convention-built-out-of-macros)
for a complete `proc_enter`/`proc_leave` pair that makes a fixed set of registers callee-saved.

## Most I/O devices are stubs

Of the 26 default ports reserved in [`io_ports.h`](../Ceres-ASM/src/vm/io_ports.h), only 7 are backed
by a working device today: the terminal (3 ports), the timer (3 ports), and system control (1 port).
Disk, GPU, mouse/gamepad, audio, and network are all reserved port ranges with no device attached —
reading them returns all-ones and writing them does nothing, exactly like any other unattached port.
See [I/O devices and ports](07-IO-Devices-and-Ports.md) for the full map.

## Gaps in `parseOperand`

A handful of literal kinds are not accepted as instruction *operands* today (they work fine as
`let`/`const` initializers, just not directly as, say, an immediate operand to an instruction):

- Float and string literals cannot be used directly as an operand.
- A `%%label` cannot start a statement outside a macro body (it can only be used *as an operand*
  outside one, referencing a hygienic label that was already defined inside some macro's expansion).

## The debugger's call stack is inferred, and its watchpoints only see writes

`ceres debug` gives breakpoints, stepping by source line, registers, memory and a call stack (see
[The debugger](22-Debugger.md)), in the terminal and in VSCode alike.

Two things it cannot do exactly rather than approximately:

- **The call stack is reconstructed, not unwound.** `CALL` pushes only a return address and nothing
  in the machine tracks frames, so the debugger builds the stack by watching instructions go past.
  A program that unwinds by hand, or jumps into the middle of a subroutine, can desynchronise it —
  which is why `bt` marks its output as reconstructed.
- **A fault is reported after the handler has been entered.** The faulting address is captured and
  shown, but execution has already been redirected by the time the debugger regains control; there
  is no way to hold the machine at the faulting instruction itself.

A **watchpoint only detects writes**, and detects them by comparing the watched bytes to a snapshot
between instructions rather than by trapping the access. A read is invisible to it, and a write that
puts back the value that was already there is too.

Running **backwards** works, but only as far as the recording reaches: `interval x snapshots`
instructions, 1.28 million by default. Older than that and the debugger says so rather than landing
somewhere else. Recording also costs one copy of the machine's memory, which is why `--no-history`
exists for a session started with a very large `--memory`.

## No indexing, and no struct types

Multidimensional arrays and `struct` give the *layout*; walking it is arithmetic you write yourself.
`grid[1][2]` is not an operand, and `let player: Entity` is not a declaration — a struct is a
generator of offset constants rather than a type (see [Structs](23-Structs.md)). `dimof` and `sizeof`
exist so that at least the numbers do not have to be repeated by hand.

## A register alias cannot be exported

`alias` is resolved by the parser, which is what makes it cost nothing downstream — but it also means
there is no `global alias`, because a parser has no imports to consult; those are resolved a stage
later. An alias is file-scoped, always.

## Things that are easy to mistake for bugs, but are intentional

These aren't gaps — they're deliberate design decisions covered elsewhere in this wiki, listed here
because a reader coming from a more conventional ISA might otherwise assume they're mistakes:

| Behaviour | Why | See |
| --- | --- | --- |
| Division/modulo by zero doesn't fault | Sets the Trap flag and continues, leaving the destination unchanged | [Instruction set → Arithmetic](05-Instruction-Set.md#arithmetic-0x10-0x28) |
| `str [rd + imm16], rs` puts the base *before* the value | `imm16` occupies the same bits as `rt`, so there's no room for a third register | [Instruction format](04-Instruction-Format.md#the-critical-overlap-imm16-and-rt) |
| `ldv`/`stv` silently clobber `r12` | Need a scratch register for the address; `r13` is the link register, so using it there would break subroutines | [Pseudo-instructions](06-Pseudo-Instructions.md#the-r12-clobber) |
| `iret` never restores the Halting flag | `halt` means "wait for an interrupt"; restoring it would put the machine straight back to sleep with no way to wake it | [Registers and flags](03-Registers-and-Flags.md#flags-register) |
| A stack overflow only protects the vector table/BIOS, not the program itself | Nothing tracks where the loaded program's image ends | [Memory → The stack](02-Memory.md#the-stack) |
| `[r5+0]` fails to parse | Lexes as the register followed by the signed literal `+0`, not as `+` then `0` | [Language syntax](10-Language-Syntax.md#addressing-memory-operands) |
| A `global const` is not in the linker's global table | It occupies no memory, so there is nothing to link; it travels by `import`, which lets two libraries declare the same name | [Labels and symbols](12-Labels-and-Symbols.md) |
| `math.PI` needs the dot adjacent | Adjacency is the only thing separating it from `jnz .loop` once whitespace is discarded | [Language syntax](10-Language-Syntax.md#qualified-names) |
| A `.cres` from before the renumbering is rejected, not run | `0x70` used to mean `PUSH` and now means `JAB`; running it would be silent corruption | [The `.cres` binary format](09-CRES-Binary-Format.md#versioning) |

## Related pages

- [Registers and flags](03-Registers-and-Flags.md)
- [Pseudo-instructions](06-Pseudo-Instructions.md)
- [I/O devices and ports](07-IO-Devices-and-Ports.md)
- [Constants and expressions](13-Constants-and-Expressions.md)
- [Structs](23-Structs.md)
- [Macros](14-Macros.md) — a worked example of a calling convention built with `proc_enter`/`proc_leave` macros.

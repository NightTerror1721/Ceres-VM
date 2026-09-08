# Known limitations

[← Back to index](README.md)

The assembler and the VM work end to end — every example in this wiki assembles and runs — but a
number of things are explicitly not done yet. This page collects them in one place so they aren't
mistaken for bugs.

## A calling convention is a contract, not a rule

Nothing in the machine enforces one, and nothing ever will: `call` pushes a return address, `ret`
pops it, and that is the whole of what the hardware knows about subroutines.

What exists now is a convention **written down** — register roles, a frame shape, and the two macros
that open and close it — in [`lib/call.casm`](../Ceres-ASM/lib/call.casm), documented in
[A calling convention](24-Calling-Convention.md) and exercised by
[`examples/calling_convention.casm`](../Ceres-ASM/examples/calling_convention.casm).

Following it is still discipline. There is no way to write a macro that verifies you preserved `r8`,
or that you left `sp` where you found it, so the assembler cannot catch a function that breaks the
contract — only you can. What the macros buy is that the disciplined thing is also the shortest thing
to write.

One hazard is worth repeating because it is invisible: **`at` (`r13`) is clobbered by `stv`, and by
`ldv` into a float register** — but only when the variable is more than ±32 KiB from the
instruction, which in a program of ordinary size is never. Inside that range the linker relaxes
them into one PC-relative word and no scratch register is needed at all. `la` never needed one, and
neither does `ldv` into an integer register.

That it depends on the distance is the part to keep in mind: a program that grows past 32 KiB of
statics starts clobbering `at` in code that did not before, without a word. Nothing has changed at
the source level, so nothing warns.

## Most I/O devices are stubs

Of the 26 default ports reserved in [`io_ports.h`](../Ceres-ASM/src/vm/io_ports.h), 15 are backed
by a working device today: the terminal (3 ports), the timer (3 ports), the disk (4 ports), the
framebuffer (4 ports), and system control (1 port). Mouse/gamepad, audio, network and the
debug-hex port are still reserved ranges with no device attached — reading them returns
all-ones and writing them does nothing, exactly like any other unattached port. The framebuffer
draws characters rather than pixels, which is as far as a VM with no window of its own can go.
See [I/O devices and ports](07-IO-Devices-and-Ports.md) for the full map.

## One gap left in `parseOperand`

Float and string literals **are** operands now: each becomes an anonymous `.rodata` declaration and
the operand becomes its name, which is what you would have had to write out by hand.

What remains: a `%%label` cannot start a statement outside a macro body. It can be used *as an
operand* out there, referring to a hygienic label some expansion already defined — but defining one
outside a macro would be a label with an expansion number and no expansion, which is not a thing
worth having.

## The debugger's call stack is inferred, and its watchpoints only see writes

`ceres debug` gives breakpoints, stepping by source line, registers, memory and a call stack (see
[The debugger](22-Debugger.md)), in the terminal and in VSCode alike.

Two things it cannot do exactly rather than approximately:

- **The call stack is only reconstructed for a function with no frame.** `CALL` pushes a return
  address and `enter` pushes the caller's `fp` on top of it, so a function that opens a frame can
  be walked exactly — the assembler records which ones do, in the debug section. Without a frame
  there is no chain, and the debugger falls back to watching instructions go past, which a program
  that unwinds by hand can desynchronise. `bt` says which of the two it did.
- **A fault is reported after the handler has been entered.** The faulting address is captured and
  shown, but execution has already been redirected by the time the debugger regains control; there
  is no way to hold the machine at the faulting instruction itself.

A **watchpoint sees the access itself** now, not its consequence: the engine reports every load and
store it performs, so a read can be watched (`watch read x`, or `watch rw x` for either) and a write
that puts back the value that was already there still stops. Instruction fetch is deliberately not
reported — it is not an access the program made.

Running **backwards** works, but only as far as the recording reaches: `interval x snapshots`
instructions, 1.28 million by default. Older than that and the debugger says so rather than landing
somewhere else. Recording also costs one copy of the machine's memory, which is why `--no-history`
exists for a session started with a very large `--memory`.

## No indexing

Multidimensional arrays and `struct` give the *layout*; walking it is arithmetic you write yourself.
`grid[1][2]` is not an operand. `dimof` and `sizeof` exist so that at least the numbers do not have
to be repeated by hand.

`let player: Entity` **is** a declaration now, and its fields can be initialised positionally — but
that is a spelling of `u8[Entity]` and nothing more (see [Structs](23-Structs.md)). A struct is
still a generator of offset constants: nothing checks that the `r2` in `ldr r1, [r2 + Entity.y]`
points at an Entity, and there is no way to say that it should.

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
| A far `stv` silently clobbers `at` (`r13`) | Out of PC-relative reach it needs a base register for the address that is not the one holding the value | [Pseudo-instructions](06-Pseudo-Instructions.md#the-at-clobber) |
| `iret` never restores the Halting flag | `halt` means "wait for an interrupt"; restoring it would put the machine straight back to sleep with no way to wake it | [Registers and flags](03-Registers-and-Flags.md#flags-register) |
| A stack overflow does not protect the heap | The limit is the end of the image, and everything above it is free ground the stack is entitled to | [Memory → The stack](02-Memory.md#the-stack) |
| `[r5+0]` fails to parse | Lexes as the register followed by the signed literal `+0`, not as `+` then `0` | [Language syntax](10-Language-Syntax.md#addressing-memory-operands) |
| A displacement above 32767 is rejected | The field is a signed 16 bits; it used to truncate silently | [Instruction format](04-Instruction-Format.md#signed-and-unsigned-immediate-fields) |
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

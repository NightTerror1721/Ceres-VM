# Interrupt vector binding

[← Back to index](README.md)

`interrupt NUMBER: handler` binds an interrupt number to a label. The linker resolves it exactly
like any other operand pair, and the loader patches the resulting address into the vector table
before the program's first instruction runs — the only way a `.casm` program can install its own
handler instead of falling through to the BIOS's default stub. See
[Interrupts and exceptions](08-Interrupts-and-Exceptions.md) for the vector table itself and why a
running program can never patch it directly.

## Syntax

```casm
interrupt UserInterrupt0: timer_isr
interrupt 17: term_isr
```

A top-level declaration, valid wherever `const` is — it needs no `@text`/`@data`/`@rodata` section,
because it emits no code or data of its own, only a binding the linker resolves.

Either side of the number is accepted:

- A literal (`interrupt 17: term_isr`).
- A named constant. The assembler predefines the reserved names from `InterruptNumber` and
  `UserInterrupt0` through `UserInterrupt47` as ordinary constants in every file, so no `import` is
  needed:

  | Name | Number | Name | Number |
  | --- | --- | --- | --- |
  | `Trap` | 1 | `AlignmentFault` | 6 |
  | `IllegalInstruction` | 2 | `PageFault` | 7 |
  | `MemoryFault` | 3 | `Syscall` | 15 |
  | `DivisionByZero` | 4 | `UserInterrupt0` … `UserInterrupt47` | 16 … 63 |
  | `StackOverflow` | 5 | | |

  `Reset` (0) is deliberately not among them — see below.

The target is a label, resolved the same way an instruction operand referencing one is: a local
`.name` from the same file, a file-level or `global` label from anywhere already in scope, or a
forward reference resolved once linking completes.

## Resolution rules

Four rules keep every binding decidable at link time:

1. **The number must fold to a constant.** `interrupt someVariable: handler` — a `let`, not a
   `const` — is rejected: *"an interrupt number must be a constant expression"*.
2. **0 is off-limits.** `interrupt 0: handler` (or `interrupt Reset: handler`) is a linker error
   naming `global main` as the only way to set the entry point:
   ```
   Linker error: interrupt 0 is the reset vector; declare 'global main' instead.
   ```
3. **One binding per number, whole-program.** Two `interrupt` declarations naming the same number —
   even from different files — is a linker error, exactly like a `global` symbol defined twice:
   ```
   Linker error: interrupt 16 is already bound to 'timer_isr' (timer.casm:3);
                 second binding to 'watchdog_isr' in wdt.casm:9.
   ```
4. **The target must be a label.** `interrupt 16: SOME_CONSTANT` is rejected: *"an interrupt handler
   must be a label"*.

Both sides go through the linker's normal operand resolution, so anything that already works for an
instruction operand works here too — a local label, a `global` one from another file, or one that
happens to be declared later in the same file.

## A worked example: waking on terminal input

`TerminalDevice::pushInput()` raises `UserInterrupt1` whenever it actually adds a byte to its input
buffer (see [I/O devices and ports](07-IO-Devices-and-Ports.md#terminaldevice-ports-0x00-0x02)).
Binding a handler for it turns the busy-wait every program used to need on `TERM_STATUS` into a real
wake-up:

```casm
interrupt UserInterrupt1: term_isr

@text
global main:
    sti                   // unmask user interrupts
    halt                  // suspended until a byte arrives (or the timer, or any other interrupt)
    // ... resumes here after term_isr's iret

term_isr:
    in   r1, TERM_IN      // pop the byte that woke us
    out  TERM_OUT, r1     // echo it straight back
    iret
```

Without the `interrupt` line, the same `sti`/`halt` still wakes up the instant input arrives — but
falls through to the BIOS's shared stub (print `E`, halt again) instead of `term_isr`, because
nothing pointed vector 17 anywhere else.

## Storage in `.cres`

Every bound vector becomes one entry in an optional section, present only when the source declared
at least one binding — announced by `ProgramHeader::flags` bit 1
(`ProgramFlags::HasInterruptVectors`), the same recipe [debug information](21-Debug-Information.md)
uses for its own optional section: additive, so a reader built before this feature existed simply
never looks past `.data` (unless bit 0 is also set, in which case it reads the debug section from
the wrong offset — which is exactly why this section sits *before* it, not after, in every build
that has both. See [The `.cres` binary format](09-CRES-Binary-Format.md#file-layout)).

| Field | Size | Meaning |
| --- | --- | --- |
| `count` | `u32` | Number of bindings that follow |
| `interruptNumber` | `u8` × `count` | 1–63 — never 0, the linker already refused that |
| `handlerAddress` | `u32` × `count` | Absolute address, already resolved — no relocation at load time |

A sparse patch list rather than a 256-byte dump of the full table, since a typical program binds one
or two vectors.

### Applying it

`CeresVM::loadProgram()` writes the entry point into vector 0 with `writeUnchecked` — the one
address a program has always had per-image control over. The patch table is the same idea, looped:
applied *after* `BIOS::initializeMemory()`, so a vector nothing bound keeps pointing at the shared
stub exactly as it does today, and each bound one overwrites its slot with the program's own
handler. A program that declares no `interrupt` bindings loads into a bit-for-bit identical memory
image to before this feature existed.

The loader re-checks what the linker already guaranteed — vector 0 is never patched, and no vector
is patched twice — because by the time a `.cres` file reaches `loadProgram()` it might not have come
from this assembler at all.

## Binding across separately-compiled objects

An `interrupt` declaration works the same when the handler lives in a different `.cobj` — see
[Separate compilation](25-Separate-Compilation.md). Assembled on its own, a unit cannot know a
handler's final address (its own sections have not been placed yet, and a handler in another object
has not even been seen), so the binding travels as an `ObjectInterruptBinding` instead: the number,
already final, plus the target described exactly the way a `Relocation` describes one — a section
and an offset for a handler in the same object, or a name for one `ceres link` has to find elsewhere.

```casm
// lib.casm
const TERM_IN  = 0xFF000008
const TERM_OUT = 0xFF000004

@text
global term_isr:
    la r13, TERM_IN
    ldrb r1, [r13 + 0]
    la r13, TERM_OUT
    strb [r13 + 0], r1
    iret
```

```casm
// main.casm
import "lib.casm"
interrupt UserInterrupt1: term_isr

@text
global main:
    sti
    halt
```

`ceres link main.cobj lib.cobj -o program.cres` resolves `term_isr` from `lib.cobj` the same pass
that resolves every other cross-object reference, and only then checks the one thing no single
object could have: that no other object bound the same number.

## Why not let a program patch the table itself?

Checked memory writes reject every address below `0x400` specifically so that a stray pointer can
never corrupt interrupt dispatch (see [Memory](02-Memory.md#protected-vs-unrestricted-access)).
Reopening that floor — the way x86 real mode's IVT works, writable by anything, no protection at all
— would give back exactly the guarantee that exists on purpose. Binding at link time and patching at
load time needs no privilege model to do it safely, because no *instruction* ever performs the
write: the table is inert data, exactly like `.rodata` is, until the loader copies it in before the
CPU takes its first step.

## Related pages

- [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — the vector table, dispatch, `halt`.
- [I/O devices and ports](07-IO-Devices-and-Ports.md) — the timer and the terminal, the two devices that raise a user interrupt today.
- [The `.cres` binary format](09-CRES-Binary-Format.md) — where the patch table sits in the file.
- [Labels and symbols](12-Labels-and-Symbols.md) — how a label reference resolves across files.
- [Memory](02-Memory.md) — why the vector table is off-limits to a running program.
- [Separate compilation](25-Separate-Compilation.md) — `ObjectInterruptBinding`, and how a binding survives being split across `.cobj` files.

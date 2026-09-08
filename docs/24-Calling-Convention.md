# A calling convention

[← Back to index](README.md)

Nothing in the machine enforces one. `call` pushes a return address, `ret` pops it, and that is the
whole of what the hardware knows about subroutines — see
[Known limitations](19-Known-Limitations.md). What follows is a contract between the code you write:
one set of rules, kept in [`lib/call.casm`](../Ceres-ASM/lib/call.casm) so it is written the same way
every time.

The complete working example is
[`examples/calling_convention.casm`](../Ceres-ASM/examples/calling_convention.casm). Everything on
this page was checked against the assembler and the engine, and the instruction-level facts say
where they come from.

```casm
import "lib/call.casm"      // proc_enter, proc_leave, and the names below
```

That one line is the whole setup. The module publishes:

| Name | What it is |
| --- | --- |
| `proc_enter <size>` | The prologue macro: `enter <size>` |
| `proc_leave` | The epilogue macro: `leave` then `ret` |
| `arg0`–`arg3`, `ret0` | `global alias` for `r0`–`r3`, and `r0` again under the name it returns in |
| `farg0`–`farg3`, `fret0` | The same for `f0`–`f3` |

The aliases are `global alias`, so importing the module is enough to write `arg0` — and declaring
the same alias again in your own file is not an error, so a file that already had its own copies
keeps working (see [Register aliases](10-Language-Syntax.md#register-aliases)).

## Register roles

| Register | Role | Survives a `call`? |
| --- | --- | --- |
| `r0`–`r3` | Arguments one to four. `r0` is also the return value. | No |
| `r4`–`r7` | Scratch | No |
| `r8`–`r11` | General purpose | **Yes** — the callee saves them |
| `r12` | Scratch — it was unusable until the assembler's temporary moved off it | No |
| `r13` / `at` | **Never use it.** See below. | — |
| `r14` / `fp` | Frame pointer, managed by `enter`/`leave` | Yes |
| `r15` / `sp` | Stack pointer | Yes |
| `f0`–`f3` | Float arguments. `f0` is also the float return value. | No |
| `f4`–`f7` | Float scratch | No |
| `f8`–`f15` | General purpose float | **Yes** |
| Flags | — | **No.** A `cmp` before a `call` is worthless after it. |

Three of these are worth saying out loud:

- **`at` is not yours.** The assembler clobbers `r13` when it has to materialise a 32-bit address:
  a `stv`, or a `ldv` whose destination is a float register. Since the linker relaxes a variable
  within ±32 KiB into a single PC-relative word, *most* of those clobber nothing in practice — but
  which ones is a property of the final layout, not of the line you wrote, so it can change when
  the program grows. Treat every `stv` and every float `ldv` as destroying `at`.
  It answers to `lr` too; that spelling is deprecated and the name it suggests is wrong, since
  nothing in the machine links through `r13`.
- **Flags are caller-saved**, which in practice means "not saved at all". Compare after the call,
  not before.
- **Returning nothing is a role too.** A function that returns no value still clobbers `r0`–`r7`,
  and the caller cannot assume otherwise.

## What these instructions actually do

The convention rests on three real instructions; nothing here is a macro pretending to be one.

| Instruction | Exactly what it does |
| --- | --- |
| `call label` | Pushes `pc + 4`, then jumps. Reaches ±8 MiB. |
| `ret` | Pops a word into `pc`. |
| `enter imm16` | Checks there is room for `4 + imm16` bytes; then pushes `fp`, sets `fp = sp`, and subtracts `imm16` from `sp`. |
| `leave` | Sets `sp = fp`, then pops `fp`. |

Two consequences to keep in mind:

- **`enter` is all or nothing.** If the whole frame does not fit, nothing is pushed and
  `StackOverflow` is raised — a prologue can never leave a function half-entered, running on a
  stack it does not own.
- **`leave` restores `sp` from `fp`**, so the frame size is written once and a function may have as
  many exit points as it likes.

Bare `enter`, with no operand, saves `fp` and reserves nothing. That is what a function wants when
it needs to read arguments five and up but has no locals of its own.

## The frame

```
higher addresses
    argument 6            [fp + 12]
    argument 5            [fp + 8]
    return address        [fp + 4]     <- pushed by `call`
    saved fp              [fp + 0]     <- pushed by `enter`; fp == sp here
    ──────────────────────────────
    outgoing args    ┐   at [sp + 0] upward
    saved registers  ├─ this function's frame, reserved by `enter <size>`
    locals           ┘
lower addresses
```

The prologue and epilogue are two macros:

```casm
import "lib/call.casm"

my_function:
    proc_enter Frame        // enter Frame
    ...
    proc_leave              // leave; ret
```

**`sp` must not move between `proc_enter` and `proc_leave`.** Everything the function needs lives in
the frame it reserved once, which is what keeps every `[sp + Frame.field]` valid for the whole body —
including across a `call`. There is no reason to push in the body: saved registers, locals and
outgoing arguments all have somewhere to live.

A function that takes four arguments or fewer, keeps everything in `r0`–`r7`, and calls nothing needs
**no frame at all** — just `ret`.

## Describing the frame with a `struct`

This is what makes the convention readable. A frame *is* a record, and
[`struct`](23-Structs.md) generates exactly the constants it needs:

```casm
struct Frame
    outgoing0: u32      // sp + 0   - becomes the callee's [fp + 8]
    saved_r8:  u32      // sp + 4
    count:     u32      // sp + 8
endstruct               // Frame == 12, and that is the size to reserve

my_function:
    proc_enter Frame
    str [sp + Frame.saved_r8], r8
    ...
    ldr r8, [sp + Frame.saved_r8]
    proc_leave
```

The struct's own name is its size, so `proc_enter Frame` reserves exactly what the fields need — one
declaration, no number repeated. Naming the fields also documents the frame, which is otherwise the
least readable part of any hand-written subroutine.

Two details the layout rule decides for you (see [Structs](23-Structs.md)): each field is aligned to
its own type, and the total is rounded up to the widest field — so a `u8` between two `u32`s costs
four bytes, not one. If you want the frame packed, order the fields yourself.

The **caller's** view of the arguments it passes is the same idea from the other side:

```casm
struct Sum6Frame
    saved_fp: u32       // fp + 0
    retaddr:  u32       // fp + 4
    arg4:     u32       // fp + 8    - the fifth argument
    arg5:     u32       // fp + 12   - the sixth
endstruct

sum6:
    enter                       // no locals; fp is only here to read the arguments
    ldr r4, [fp + Sum6Frame.arg4]
    ...
```

## Passing arguments

The first four go in `r0`–`r3`. Beyond that, the caller writes them into the **outgoing area at the
bottom of its own frame**, starting at `[sp + 0]`:

```casm
struct MainFrame
    outgoing0: u32      // becomes the callee's [fp + 8]
    outgoing1: u32      // becomes the callee's [fp + 12]
endstruct

    proc_enter MainFrame
    li  arg0, 1
    li  arg1, 2
    li  arg2, 3
    li  arg3, 4
    li  r4, 5
    str [sp + MainFrame.outgoing0], r4
    li  r4, 6
    str [sp + MainFrame.outgoing1], r4
    call sum6
```

Offset 0 is not a detail: `call` pushes the return address immediately below `sp`, and the callee's
`enter` puts the saved `fp` immediately below that. So the caller's `[sp + 0]` *is* the callee's
`[fp + 8]`.

This is why the body must not move `sp` — the outgoing area has to be exactly where `call` leaves it.

The outgoing area is sized for the **largest** call the function makes, and reused by all of them:
a function that calls one routine with six arguments and another with five reserves two words, not
three.

Float arguments go in `f0`–`f3` and are counted separately: a function taking `(u32, f32, u32)`
receives them in `r0`, `f0`, `r1`. Beyond four of either, the stack area is words, so a `f32` takes
one slot like anything else.

## Returning a value

`r0` for an integer or an address, `f0` for a float — `ret0` and `fret0` under the convention's own
names. Anything larger than a word is returned by the caller passing a pointer to space it owns,
which is not a rule the machine knows anything about; it is just an argument.

## Why the convention does not use `pushm`

[`pushm`/`popm`](05-Instruction-Set.md#pushm-and-popm) save a whole register mask in one
instruction, which looks like exactly what a prologue wants. They are deliberately not part of this
convention, because either placement breaks the frame:

- **Before `enter`**, the pushed registers land between the return address and the saved `fp`. The
  callee's `[fp + 4]` is then the last register pushed, not the return address, and arguments five
  and up move by four bytes per register saved.
- **After `enter`**, `sp` has moved, so every `[sp + Frame.field]` in the body is off by the same
  amount — including the outgoing area the next `call` will read.

`pushm` earns its keep where there is no frame to disturb: an interrupt handler saving whatever it
is about to use, or a leaf spilling a couple of registers around a single computation. Inside a
function that opened a frame, `str [sp + Frame.saved_r8], r8` costs the same four bytes per register
and keeps every offset true.

## Where the stack is

- It starts at the **top of the program's own region**, which is the end of memory minus the 1 KiB
  reserved for interrupt handlers (`Memory::SystemStackSize`). `sp` on entry is that address; there
  is deliberately no `__stack_top` symbol, because how much memory there is is chosen at run time
  with `--memory`.
- It grows **down**, and its floor is the end of the loaded image — the same address as
  `__bss_end` and `__heap_start`. Growing past it raises `StackOverflow` instead of quietly eating
  `.text`; see [Memory → The stack](02-Memory.md#the-stack).
- **An interrupt does not run on your stack.** A handler switches to the system stack above the
  program's, so a frame is never disturbed by one arriving, and an interrupt can still be taken
  when the program's own stack is nearly full — see
  [Interrupts and exceptions](08-Interrupts-and-Exceptions.md).
- The loader refuses to start a program that does not have 1 KiB of stack left over after the image
  (`minimumStack` in the [`.cres` header](09-CRES-Binary-Format.md)).

## Worked example: recursion

Recursion is where a callee-saved register earns its keep. `n` has to survive the recursive call, and
every caller-saved register is fair game across one:

```casm
struct FactFrame
    saved_r8: u32
endstruct

factorial:
    proc_enter FactFrame
    str [sp + FactFrame.saved_r8], r8

    mov  r8, arg0
    ifle r8, 1, .base

    sub  arg0, r8, 1
    call factorial              // r8 survives; r0-r7 do not
    mul  ret0, ret0, r8
    jp   .done

.base:
    li ret0, 1

.done:
    ldr r8, [sp + FactFrame.saved_r8]
    proc_leave
```

Note `ifle` rather than `cmp` + `jle`: the comparison and the branch as one instruction, and the
*signed* form, because a count is a signed quantity (see
[Instruction set](05-Instruction-Set.md#comparison-jumps--0x680x77)).

## Leaves can skip all of this

A function that calls nothing has no reason to touch the stack, and
[`bl`](05-Instruction-Set.md#branch-and-link--0x780x79) lets it not:

```casm
    bl r11, print_char      // the return address goes in r11, not on the stack

print_char:
    outb TERM_OUT, arg0
    jp r11
```

The link register is an operand, so this convention does not have to reserve one: pick any
caller-saved register the call site can spare. What it does have to say is that **the callee may
clobber it**, exactly like any other caller-saved register — so the register you link through must
not hold anything else across the call.

The labelled form reaches ±512 KiB, because `rd` costs the displacement four bits; `blr rd, rs`
takes the target in a register and has no such limit.

This only works for leaves. The moment `print_char` calls something itself, its own link is gone
unless it saves it, and saving it is the bookkeeping `call`/`ret` already does for free.

## The recipe, end to end

To write a function that follows the convention:

1. **Decide whether it needs a frame at all.** Four arguments or fewer, everything in `r0`–`r7`, and
   no calls — then just `ret`, and consider being called with `bl` instead.
2. **Write the frame as a `struct`**, in this order: outgoing arguments first (they must start at
   offset 0), then the registers you will save, then locals.
3. **`proc_enter YourFrame`** as the first instruction, or bare `enter` if the frame is empty but you
   need to read arguments five and up.
4. **Save every `r8`–`r11` and `f8`–`f15` you touch**, with `str [sp + YourFrame.saved_rN], rN`.
5. **Read arguments five and up at `[fp + 8]`, `[fp + 12]`, …**, which is where the caller left them.
6. **Do not move `sp`.** No `push`, no `pop`, no `sub sp`.
7. **Restore what you saved**, then `proc_leave`.

And to call one:

1. Put the first four arguments in `arg0`–`arg3` (`farg0`–`farg3` for floats).
2. Write the rest into your own outgoing area at `[sp + 0]` upward.
3. `call` it.
4. Assume `r0`–`r7`, `r12`, `f0`–`f7`, `at` and the flags are gone. Read the result from `ret0`.

## What still is not enforced

- **Nothing checks any of this.** There is no way to write a macro that verifies you preserved `r8`,
  or that you did not move `sp`. It is discipline, and the macros exist to make the disciplined thing
  the easy thing. What you *can* check at assembly time is arithmetic about the frame:
  `assert Frame % 4 == 0` is a real directive (see
  [Constants and expressions](10-Language-Syntax.md#directives)).
- **Stack overflow is caught at the end of the image**, so deep recursion faults instead of
  overwriting your own `.text`. What it cannot tell you is *which* function went too deep; the fault
  names an address, not a frame.
- **The debugger unwinds exactly where there is a frame to unwind.** A function that opens one with
  `enter` leaves a chain — the saved `fp` at `[fp + 0]` and the return address at `[fp + 4]` — and
  the assembler records that it does, so the call stack is walked rather than guessed. A function
  with no frame has no chain, and the stack falls back to being inferred from the calls gone past;
  those frames are marked as such (greyed in the editor's call stack). Following this convention is
  what makes the exact answer available. See [The debugger](22-Debugger.md).

## Related pages

- [Registers and flags](03-Registers-and-Flags.md) — what each register is, and the `at` clobber.
- [Instruction set](05-Instruction-Set.md#enter-and-leave) — what `enter` and `leave` do.
- [Structs](23-Structs.md) — the offset constants a frame is described with, and the layout rule.
- [Language syntax](10-Language-Syntax.md#register-aliases) — `alias`, and how `global alias` crosses a file.
- [Macros](14-Macros.md) — how `proc_enter`/`proc_leave` are defined and exported.
- [Memory](02-Memory.md#the-stack) — where the stack lives and what happens when it runs out.
- [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — why a handler does not disturb your frame.

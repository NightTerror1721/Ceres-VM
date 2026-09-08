# A calling convention

[← Back to index](README.md)

Nothing in the machine enforces one. `call` pushes a return address, `ret` pops it, and that is the
whole of what the hardware knows about subroutines — see
[Known limitations](19-Known-Limitations.md). What follows is a contract between the code you write:
one set of rules, kept in [`lib/call.casm`](../Ceres-ASM/lib/call.casm) so it is written the same way
every time.

The complete working example is
[`examples/calling_convention.casm`](../Ceres-ASM/examples/calling_convention.casm).

## Register roles

| Register | Role | Survives a `call`? |
| --- | --- | --- |
| `r0`–`r3` | Arguments one to four. `r0` is also the return value. | No |
| `r4`–`r7` | Scratch | No |
| `r8`–`r11` | General purpose | **Yes** — the callee saves them |
| `r12` | Scratch — it was unusable until the assembler's temporary moved off it | No |
| `r13` / `at` | **Never use it.** `stv`, and `ldv` into a float register, clobber it. | — |
| `r14` / `fp` | Frame pointer, managed by `enter`/`leave` | Yes |
| `r15` / `sp` | Stack pointer | Yes |
| `f0`–`f3` | Float arguments. `f0` is also the float return value. | No |
| `f4`–`f7` | Float scratch | No |
| `f8`–`f15` | General purpose float | **Yes** |
| Flags | — | **No.** A `cmp` before a `call` is worthless after it. |

Three of these are worth saying out loud:

- **`at` is not yours.** An `stv` anywhere in your function destroys it, even if the instruction
  mentions neither `at` nor anything near it. It answers to `lr` too, but that spelling is
  deprecated and the name it suggests is wrong: nothing in the machine links through `r13`.
- **Flags are caller-saved**, which in practice means "not saved at all". Compare after the call, not
  before.

## The frame

```
higher addresses
    argument 6            [fp + 12]
    argument 5            [fp + 8]
    return address        [fp + 4]     <- pushed by `call`
    saved fp              [fp + 0]     <- pushed by `enter`; fp == sp here
    ──────────────────────────────
    saved registers  ┐
    locals           ├─ this function's frame, [sp + 0] upward
    outgoing args    ┘   (outgoing args first, at offset 0)
lower addresses
```

The prologue and epilogue are two macros:

```casm
import "lib/call.casm"

my_function:
    proc_enter Frame        // enter; sub sp, sp, Frame
    ...
    proc_leave             // leave; ret
```

`leave` restores `sp` from `fp`, so the frame size is never repeated and a function may have as many
exit points as it likes.

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
    str r8, [sp + Frame.saved_r8]
    ...
    ldr r8, [sp + Frame.saved_r8]
    proc_leave
```

The struct's own name is its size, so `proc_enter Frame` reserves exactly what the fields need — one
declaration, no number repeated. Naming the fields also documents the frame, which is otherwise the
least readable part of any hand-written subroutine.

The **caller's** view of the arguments it passes is the same idea from the other side:

```casm
struct Sum6Frame
    saved_fp: u32       // fp + 0
    retaddr:  u32       // fp + 4
    arg4:     u32       // fp + 8    - the fifth argument
    arg5:     u32       // fp + 12   - the sixth
endstruct

sum6:
    enter
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
    li  r0, 1
    li  r1, 2
    li  r2, 3
    li  r3, 4
    li  r4, 5
    str r4, [sp + MainFrame.outgoing0]
    li  r4, 6
    str r4, [sp + MainFrame.outgoing1]
    call sum6
```

Offset 0 is not a detail: `call` pushes the return address immediately below `sp`, and the callee's
`enter` puts `fp` immediately below that. So the caller's `[sp + 0]` *is* the callee's `[fp + 8]`.

This is why the body must not move `sp` — the outgoing area has to be exactly where `call` leaves it.

## Naming the argument registers

```casm
alias arg0 = r0
alias arg1 = r1
alias arg2 = r2
alias arg3 = r3
alias ret0 = r0
```

[`alias`](10-Language-Syntax.md#register-aliases) is resolved by the parser, so it is **file-scoped
and cannot be exported** — `lib/call.casm` cannot provide these. Five lines copied into each file
that uses the convention is the price.

## Worked example: recursion

Recursion is where a callee-saved register earns its keep. `n` has to survive the recursive call, and
every caller-saved register is fair game across one:

```casm
struct FactFrame
    saved_r8: u32
endstruct

factorial:
    proc_enter FactFrame
    str r8, [sp + FactFrame.saved_r8]

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

## What still is not enforced

- **Nothing checks any of this.** There is no way to write a macro that verifies you preserved `r8`,
  or that you did not move `sp`. It is discipline, and the macros exist to make the disciplined thing
  the easy thing.
- **Stack overflow is caught at the end of the image**, so deep recursion faults instead of
  overwriting your own `.text` — see [Memory → The stack](02-Memory.md#the-stack). What it cannot
  tell you is *which* function went too deep; the fault names an address, not a frame.
- **The debugger's call stack is reconstructed, not unwound** (see [The debugger](22-Debugger.md)).
  Following this convention makes that reconstruction more likely to be right, but does not make it
  exact.

## Related pages

- [Registers and flags](03-Registers-and-Flags.md) — what each register is, and the `at` clobber.
- [Pseudo-instructions](06-Pseudo-Instructions.md#enter-and-leave--stack-frames) — what `enter` and `leave` expand to.
- [Structs](23-Structs.md) — the offset constants a frame is described with.
- [Language syntax](10-Language-Syntax.md#register-aliases) — `alias`, and why it does not cross a file.
- [Macros](14-Macros.md) — how `proc_enter`/`proc_leave` are defined and exported.
- [Memory](02-Memory.md#the-stack) — where the stack lives and what happens when it runs out.

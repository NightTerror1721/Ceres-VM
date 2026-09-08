# Pseudo-instructions

[← Back to index](README.md)

A pseudo-instruction is a mnemonic that doesn't correspond to a single real opcode: the assembler
expands it into a short, fixed sequence of real instructions. From
[`instruction_info.cpp`](../Ceres-ASM/src/assembler/instruction_info.cpp):

### Addresses and constants

| Written | Expands to | Size |
| --- | --- | --- |
| `la rd, symbol` | `lui` + `ori` | 8 bytes |
| `lc rd, imm32` | `lui` + `ori` | 8 bytes |
| `ldv rd, variable` | `lui` + `ori` + a load chosen by the variable's declared type | 12 bytes |
| `stv rs, variable` | `lui` + `ori` + a store chosen by the variable's declared type | 12 bytes |

### Arithmetic and moves

| Written | Expands to | Size |
| --- | --- | --- |
| `neg rd, rs` | `imul rd, rs, -1` (integer), or `fneg` (float registers) | 4 bytes |
| `inc rd` | `addi rd, rd, 1` | 4 bytes |
| `dec rd` | `subi rd, rd, 1` | 4 bytes |
| `clr rd` | `li rd, 0` | 4 bytes |
| `swap rd, rs` | three `xor`s | 12 bytes |

### Control flow

| Written | Expands to | Size |
| --- | --- | --- |
| `jmp target` | `jp` | 4 bytes |
| `jeq target` / `jne target` | `jz` / `jnz` | 4 bytes |
| `tst rs` | `cmpi rs, 0` | 4 bytes |
| `ifXX a, b, target` | `cmp`/`cmpi`/`fcmp` + the matching branch | 8 bytes |

### Stack frames

| Written | Expands to | Size |
| --- | --- | --- |

## Why `la`/`ldv`/`stv` need two instructions

The widest immediate field in the 32-bit instruction encoding is 16 bits (see
[Instruction format](04-Instruction-Format.md)), so there is no single instruction that can load a
full 32-bit address. Building one takes:

```casm
lui rd, HIGH16(addr)   // rd = addr's upper 16 bits, shifted left by 16
ori rd, rd, LOW16(addr) // rd |= addr's lower 16 bits
```

`la` (Load Address) is exactly this pair, computing the address of a label or variable without
dereferencing it — the assembly-level equivalent of `&symbol` in C.

## `la` — Load Address

```casm
la rd, symbol
```

`symbol` can be a label, a variable (of any scalar or array type, sized or unsized), or a constant
that names an address. The result in `rd` is the symbol's absolute address; nothing is read from
memory. This is what you use to get a pointer you'll walk yourself:

```casm
@rodata
    let greeting: u8[16] = "Hello, CeresVM!"

@text
global main:
    la r1, greeting     // r1 = address of the string
    ldrb r2, [r1]       // r2 = greeting[0] = 'H'
```

## `ldv` — Load Variable

```casm
ldv rd, variable
```

Loads the *value* stored at `variable`'s address, choosing the right-width load instruction from the
variable's declared scalar type:

| Variable's declared type | Real load instruction chosen |
| --- | --- |
| `u8` | `ldrb` |
| `i8` | `ldrsb` |
| `u16` | `ldrh` |
| `i16` | `ldrsh` |
| `u32` | `ldr` |
| `i32` | `ldr` |
| `f32` | `fldr` (into a float register) |

```casm
@data
    let counter: u32 = 0

@text
global main:
    ldv r1, counter     // r1 = counter's current value (not its address)
```

`ldv` is only defined for **scalar** variable references, not arrays — to read one element out of an
array you compute the address yourself (typically with `la` plus arithmetic) and use a plain `ldr`/
`ldrb`/etc.

## `stv` — Store Variable

```casm
stv rs, variable
```

The mirror of `ldv`: stores `rs` into `variable`'s address, again picking the store width from the
declared type (`strb`/`strh`/`str`/`fstr`).

```casm
@data
    let counter: u32 = 0

@text
global main:
    ldv r1, counter
    add r1, r1, 1
    stv r1, counter     // counter = counter + 1
```

## The `at` clobber

Materialising a 32-bit address takes a `lui`+`ori` pair, and that pair has to land somewhere. Most of
the time it lands in a register the instruction was already given, and nothing else is disturbed:

| Pseudo-instruction | Forms | Needs a scratch? | Why |
| --- | --- | --- | --- |
| `la rd, ...` | 7 | **No** | Both halves target `rd`. No form of `la` touches a scratch register, including the one for an `f32` variable. |
| `ldv rd, var` | 6 | **No** | Builds the address in `rd`, then overwrites `rd` with the value loaded through it. |
| `ldv fd, var` | 1 | **Yes** | The destination is in the float bank, so it cannot hold the integer address on the way. |
| `stv rs, var` | 7 | **Yes** | The register holding the value cannot also be the base to store through. |

So the hazard is exactly **`stv`, and `ldv` into a float register** — eight expansions out of
twenty-one. Those borrow `r13`, the assembler temporary, spelled **`at`** — see the comment in
[`instruction_info.cpp`](../Ceres-ASM/src/assembler/instruction_info.cpp):

> Register the assembler is allowed to clobber while materialising a 32-bit address. Only `STV` and
> the float form of `LDV` need one: every other expansion builds the address in the operand register
> it was handed.

Practical consequence: **never rely on `at` surviving an `stv`**, even when neither its source nor
its destination register is `at` itself.

```casm
li  at, 42
stv r1, counter     // at is now an address, NOT 42 anymore
```

### Why `r13` and not `r12`

It was `r12` until it wasn't, on the grounds that `r13` was the Link Register and using it would make
an `stv` inside a subroutine destroy its own return address. That describes a machine Ceres has never
been: `CALL` pushes the return address on the stack and `RET` pops it, and nothing anywhere reads
`r13`. The register was a general-purpose one with a name that promised something the hardware did
not do.

Moving the clobber onto it costs no registers — thirteen were usable before and thirteen are usable
now — but it puts the hazard on the one register that has a name to warn you with, and it leaves
`r0`–`r12` contiguous and all yours. `lr` is still accepted as a spelling of `r13` so that older
sources keep assembling; `at` is what it should be called.

## `ldv` and `stv` have two sizes

`ldv` and `stv` always work: `lui`, `ori`, and the load or store, with `at` borrowed to hold the
address while the value goes somewhere else. Twelve bytes, and a register the programmer did not
name gone. Most variables never needed any of that. `.rodata`, `.data` and `.bss` sit immediately
after `.text`, so in a program of any ordinary size a static is a few hundred bytes from the
instruction that reads it.

When the variable is within **±32 KiB** of the instruction, the linker rewrites the pseudo-
instruction into a single PC-relative word, with the address as a displacement measured from the
instruction itself the way a branch's is:

```casm
@data
    let counter: u32 = 0

@text
global main:
    ldv r1, counter         // LDRP r1, [pc + 24] - one word
    inc r1
    stv r1, counter         // STRP [pc + 16], r1 - and `at` is untouched
```

Out of that range it stays the three-word form, and only then does it clobber `at`.

### How the linker gets to choose

An instruction's size is fixed before anything knows where the variable will be: the translation-unit
pass adds up section sizes as it walks the statements, and needs each instruction's size to do it.
The address that decides whether the short form reaches is not known until every unit has been laid
out. So the linker lays the program out **twice** — once to find out where everything is, then again
once it knows which instructions can shrink.

Once, not repeatedly, and that is the part worth knowing. Relaxation normally needs a fixpoint:
shorten, lay out again, discover something that no longer reaches, grow it back. Here it does not,
because the layout puts all of `.text` before all of `.rodata`, `.data` and `.bss`, so **every
reference from an instruction to a variable points forward**.

Take an instruction at `A` naming a variable at `D > A`. Shortening instructions *before* `A` lowers
`A` and `D` by the same amount, and the distance does not change. Shortening instructions *after*
`A` lowers only `D`, and the distance shrinks. Shortening never moves anything further away — so
whatever reaches on the first, pessimistic layout still reaches on the second, and measuring once is
enough.

The second pass has to start over from the same place the first one did, which is why the linker
snapshots symbol addresses **and operands** before it begins. A resolved operand has forgotten the
name it resolved and cannot be asked again, and after a relayout every address it holds is wrong —
not only the ones that moved.

### Asking for the short form by name

`ldvp` and `stvp` are the same encoding, demanded rather than hoped for: a variable out of reach is
an error instead of a longer instruction.

```
'LDVP Reg VarU32' is 40968 bytes away, out of reach for a PC-relative access; use ldv/stv instead
```

Write them where four bytes is a requirement — a hot loop, an interrupt handler with a budget —
rather than a preference. Everywhere else `ldv` and `stv` already do the right thing.

## Fixed instruction size, and the one exception

Every other pseudo-instruction has exactly one size, decided from its mnemonic during the
translation-unit pass, before linking — see
[CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md). Where a mnemonic has overloads that
compile to different numbers of words, the assembler reserves the largest and pads the rest with
`nop`.

`ldv` and `stv` are the exception, and the only one: their size is decided by the *linker*, which
is the first thing that knows how far away the variable is. That is what relaxation buys, and it
is the mechanism any future short form would use — a `bl` chosen over a `call` within reach, say,
or a short branch.

## `neg` — Negate

```casm
neg rd, rs      // integer registers: expands to `imul rd, rs, -1`
neg fd, fs      // float registers: expands to `fneg fd, fs`
```

There is no dedicated integer negate opcode; multiplying by `-1` is cheaper to implement in the
instruction table than adding a new opcode, and produces identical flags to a real signed multiply
by `-1` (see [Instruction set → Arithmetic](05-Instruction-Set.md#arithmetic-0x10-0x28)). The
floating-point form does have a dedicated real opcode (`FNEG`, `0x28`), so `neg f0, f1` compiles to a
single instruction, not two.

## `lc` — Load Constant

`la` materialises the address of a *symbol*; `lc` does the same for a plain 32-bit **value**:

```casm
    li r1, 0x1234           // fits in 16 bits, one instruction
    lc r1, 0x12345678       // does not: lui r1, 0x1234 then ori r1, r1, 0x5678
```

`li` only reaches 16 bits and `la` only takes symbols, so before `lc` a full-width constant had to
be written as the `lui`/`ori` pair by hand. Unlike `stv` it needs no scratch register: both
halves target `rd` directly.

## Comparison and branch in one: `ifXX`

`ifXX a, b, target` writes the comparison and the branch together. There are ten, one per condition,
matching the jumps in [Instruction set](05-Instruction-Set.md#comparison-jumps--0x680x77):

| Signed | Unsigned | Meaning |
| --- | --- | --- |
| `ifeq` | `ifeq` | `a == b` |
| `ifne` | `ifne` | `a != b` |
| `ifgr` | `ifab` | `a > b` |
| `ifge` | `ifae` | `a >= b` |
| `ifls` | `ifbl` | `a < b` |
| `ifle` | `ifbe` | `a <= b` |

The second operand may be a register or an immediate, and a pair of float registers picks `FCMP`:

```casm
    ifls r1, r2, .smaller       // cmp  r1, r2  + jls .smaller
    ifge r3, 100, .at_least     // cmpi r3, 100 + jge .at_least
    ifgr f0, f1, .bigger        // fcmp f0, f1  + jab .bigger
```

The float form is the one worth reaching for: `FCMP` clears Overflow and puts `fs < ft` in Carry, so
the *unsigned* branch is the correct one after it — `ifXX` picks it so you do not have to remember.

Both words are emitted at consecutive addresses, and the branch's displacement is measured from its
own address, so an `ifXX` behaves exactly like the `cmp`/jump pair written out.

## `enter` and `leave` — not pseudo-instructions any more

They were `push fp` + `mov fp, sp` and its inverse. They are now real opcodes that also open and
close the frame, so a whole prologue is one word — see
[Instruction set → `enter` and `leave`](05-Instruction-Set.md#enter-and-leave).

```casm
some_function:
    enter Frame             // push fp; fp = sp; sp -= Frame
    ...
    leave                   // sp = fp; pop fp
    ret
```

They are still the only two things in the project that touch `fp` (`r14`), and they still do not
make a calling convention on their own — nothing saves argument registers, and `ret` still pops the
address `call` pushed. What they do is make the frame-pointer half of one identical every time.

## `swap` — exchange without a temporary

```casm
swap r3, r4
```

Three `xor`s, so it needs no scratch register and never touches one the programmer did not name:

```casm
xor r3, r3, r4
xor r4, r3, r4
xor r3, r3, r4
```

Twelve bytes against the eight of `mov`+`mov` through a spare register — the trade is code size for
not having a spare register to spend.

## The small ones

```casm
inc r1          // addi r1, r1, 1
dec r1          // subi r1, r1, 1
clr r1          // li r1, 0
tst r1          // cmpi r1, 0
jmp .done       // jp .done
```

`tst` exists to make the branch after it readable: `tst r1` then `jz` says "if r1 is zero" more
plainly than `cmp r1, 0` does, and costs the same single instruction.

## Related pages

- [Instruction format](04-Instruction-Format.md) — why no single instruction can hold a 32-bit immediate.
- [Instruction set](05-Instruction-Set.md) — the real opcodes these expand into.
- [Registers and flags](03-Registers-and-Flags.md) — the role of `at`/`fp`/`sp`.
- [Constants and expressions](13-Constants-and-Expressions.md) — what can be written as the immediate of an `lc` or an `ifXX`.

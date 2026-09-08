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
| `enter` | `push fp` + `mov fp, sp` | 8 bytes |
| `leave` | `mov sp, fp` + `pop fp` | 8 bytes |

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

## `ldvp` and `stvp` — the near forms

`ldv` and `stv` always work and always cost twelve bytes: `lui`, `ori`, and the load or store, with
`at` borrowed to hold the address while the value goes somewhere else. Most variables do not need
any of that. `.rodata`, `.data` and `.bss` sit immediately after `.text`, so in a program of any
ordinary size a static is a few hundred bytes from the instruction that reads it.

`ldvp` and `stvp` take the address as a **displacement from the instruction itself**, the way a
branch does:

```casm
@data
    let counter: u32 = 0

@text
global main:
    ldvp r1, counter        // one word, LDRP r1, [pc + 36]
    inc  r1
    stvp r1, counter        // one word, and `at` is untouched
```

The scalar type picks the width exactly as it does for `ldv`/`stv`, so `ldvp` into a float register
assembles to `FLDRP` and an `i8` variable to `LDRSBP`.

**Reach is ±32 KiB.** Further than that is a link error naming the distance:

```
'LDVP Reg VarU32' is 40968 bytes away, out of reach for a PC-relative access; use ldv/stv instead
```

### Why not just make `ldv` do this

Because an instruction's size is fixed before anything knows where the variable will be. The
translation-unit pass adds up section sizes as it walks the statements, and it needs each
instruction's size to do it; the address that would decide whether the near form reaches is not
known until the linker has laid out every unit. Choosing per instruction would need a relaxation
pass — lay out optimistically, grow whatever does not reach, lay out again until nothing changes.
That is a real and well-understood technique, and it is a change to the assembler's central
invariant rather than a detail of these instructions. Separate mnemonics keep the choice explicit
and the sizes knowable.

## Fixed instruction size, even when the variant is shorter

`ldv`/`stv` pick their trailing load/store instruction based on the variable's *scalar type*, but the
assembler has to decide **how many bytes the whole pseudo-instruction occupies** before it knows
final addresses (this happens during the translation-unit pass, before linking — see
[CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md)). Since every variant of `ldv`/`stv`
compiles to exactly 3 real instructions (`lui`+`ori`+ load-or-store) regardless of the scalar type,
this isn't actually a problem in practice — every `ldv`/`stv` is always 12 bytes. The size is
reserved up front and never needs padding.

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

## `enter` and `leave` — stack frames

```casm
some_function:
    enter                   // push fp; mov fp, sp
    ...
    leave                   // mov sp, fp; pop fp
    ret
```

These are the only instructions in the project that touch `fp` (`r14`), which is otherwise defined
and unused (see [Registers and flags](03-Registers-and-Flags.md)). They do not make a calling
convention on their own — nothing saves argument registers, and `ret` still pops the address `call`
pushed — but they make the frame-pointer half of one something the assembler writes identically
every time.

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

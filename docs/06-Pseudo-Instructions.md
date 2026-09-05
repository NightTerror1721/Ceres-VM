# Pseudo-instructions

[← Back to index](README.md)

A pseudo-instruction is a mnemonic that doesn't correspond to a single real opcode: the assembler
expands it into a short, fixed sequence of real instructions. From
[`instruction_info.cpp`](../Ceres-ASM/src/assembler/instruction_info.cpp):

| Written | Expands to | Size |
| --- | --- | --- |
| `la rd, symbol` | `lui` + `ori` | 8 bytes |
| `ldv rd, variable` | `lui` + `ori` + a load chosen by the variable's declared type | 12 bytes |
| `stv rs, variable` | `lui` + `ori` + a store chosen by the variable's declared type | 12 bytes |
| `neg rd, rs` | `imul rd, rs, -1` (integer), or `fneg` (float registers) | 4 bytes |

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

## The `r12` clobber

`ldv` and `stv` both need a scratch register to hold the address they compute internally (the `lui`+
`ori` pair), separate from the register you asked to load into or store from. That scratch register
is **always `r12`** — see the comment in
[`instruction_info.cpp`](../Ceres-ASM/src/assembler/instruction_info.cpp):

> Register the assembler is allowed to clobber while materializing a 32-bit address for the
> `LDV`/`STV` pseudo-instructions. `R13` is the Link Register, so using it made any `STV` inside a
> subroutine destroy its own return address; `R12` is the last general-purpose register.

Practical consequence: **never rely on `r12` surviving an `ldv` or `stv`**, even when neither its
source nor destination register is `r12` itself.

```casm
li r12, 42
ldv r1, some_variable   // r12 is now overwritten with an address, NOT 42 anymore
```

`la` does **not** clobber `r12` when its destination is a different register — its two instructions
(`lui`+`ori`) both target `rd` directly, no scratch register is needed. The clobber only applies to
`ldv`/`stv`, which need somewhere to hold the address *while also* loading/storing through a
possibly-different register.

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

## Related pages

- [Instruction format](04-Instruction-Format.md) — why no single instruction can hold a 32-bit immediate.
- [Instruction set](05-Instruction-Set.md) — the real opcodes these expand into.
- [Registers and flags](03-Registers-and-Flags.md) — the role of `r12`/`r13`/`r14`/`r15`.

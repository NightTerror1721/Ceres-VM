# Registers and flags

[← Back to index](README.md)

## Integer registers

There are 16 general-purpose 32-bit registers, `r0`–`r15`. Four of them carry a name and a
role beyond "general purpose" — though the VM only actually special-cases one of them (`sp`):

| Register | Alias | Role |
| --- | --- | --- |
| `r0`–`r11` | — | General purpose. |
| `r12` | — | General purpose, with nothing special about it. |
| `r13` | `at` (Assembler Temporary) | **The assembler uses it as scratch space** when materializing a full 32-bit address for `stv`, and for `ldv` into a float register (see [Pseudo-instructions](06-Pseudo-Instructions.md)). Do not expect its value to survive one of those. Nothing in the VM itself touches it: `call`/`CALL` push the return address on the stack and `ret`/`RET` pop it, so there is no link register here. `lr` is still accepted as a name for `r13`, but it is the deprecated spelling. |
| `r14` | `fp` (Frame Pointer) | No *real* opcode reads or writes it specially, but the `enter` and `leave` [pseudo-instructions](06-Pseudo-Instructions.md#enter-and-leave--stack-frames) do — they are the only things in the project that name it by role. Beyond those, it behaves exactly like `r0`–`r11`. |
| `r15` | `sp` (Stack Pointer) | Initialized to the top of memory (`memory.size()`) on reset. `PUSH`/`POP`/`CALL`/`RET`/`PUSHF`/`POPF`/interrupt dispatch all read and write it directly. |

The aliases come from `RIndex` in [`registers.h`](../Ceres/libs/core/include/ceres/core/isa/registers.h):

```cpp
enum class RIndex : u8 { R0=0, ..., R12=12, R13=13, R14=14, R15=15, AT=R13, FP=R14, SP=R15 };
```

`GeneralPurposeRegisterPool` stores all 16 as a `std::array<Register, 16>`; `Register` itself is a
thin wrapper around a `u32` with arithmetic, bitwise and comparison operators, plus explicit
signed/unsigned conversion helpers.

## Floating-point registers

There are also 16 floating-point registers, `f0`–`f15`, each holding a 32-bit IEEE-754 `float`
(`FloatingPointRegisterPool`, in [`fregisters.h`](../Ceres/libs/core/include/ceres/core/isa/fregisters.h)).

**Floating-point registers share the exact same bit fields in the instruction encoding as integer
registers.** There is no separate register-index space: `f3` in an operand position occupies the
same 4-bit field that `r3` would. What decides whether an instruction operates on integers or
floats is purely the *opcode* — `ADD` reads/writes the integer bank, `FADD` reads/writes the float
bank, and both use identical `rd`/`rs`/`rt` bit positions (renamed `fd`/`fs`/`ft` for readability).
See [Instruction format](04-Instruction-Format.md).

`MTF`/`mtf` and `MFF`/`mff` move the raw 32-bit *bit pattern* between the two banks without any
numeric conversion (i.e., they reinterpret the bits, they don't call `(float)` on an int). Use
`ITOF`/`itof`, `IITOF`/`iitof`, `FTOI`/`ftoi`, `FTOII`/`ftoii` for actual numeric conversion — see
[Instruction set → Conversions](05-Instruction-Set.md#conversions).

## Writing a register

`r0`–`r15` and `f0`–`f15`, and the three that have a role also answer to it: `sp`, `fp` and `at`
(`r15`, `r14`, `r13`). Names are case-insensitive.

```casm
    ldr r1, [sp + 8]
    mov fp, sp
```

## Naming registers

A register can be given a name for readability, which is purely lexical and file-scoped:

```casm
alias cursor = r5
alias acc    = f2
```

By the time anything downstream sees the operand it is an ordinary register, so nothing here changes
— see [Language syntax → Register aliases](10-Language-Syntax.md#register-aliases).

## Flags register

`FlagRegister` (also a `Register` subclass) holds seven single-bit flags, defined in `ExecutionFlag`:

| Flag | Bit | Set by | Read by |
| --- | --- | --- | --- |
| Zero (`ZF`) | `1<<0` | Arithmetic, logic, `cmp`/`CMP`/`CMPI`/`fcmp` | `jz`, `jnz` |
| Sign (`SF`) | `1<<1` | Arithmetic, logic, `cmp` | `js`, `jns` |
| Carry (`CF`) | `1<<2` | Arithmetic, shifts, `cmp` | `jc`, `jnc`, `adc`, `sbc` |
| Overflow (`OF`) | `1<<3` | Signed arithmetic | `jo`, `jno` |
| Interrupt (`IF`) | `1<<4` | `sti` / `cli` | Interrupt dispatch: interrupts 16–63 (user interrupts) are dropped while it's clear; interrupts 0–15 (reserved/system) are always deliverable regardless of this flag. |
| Halting (`HF`) | `1<<5` | `halt` | The `step()` loop, to decide whether to actually fetch/execute or just tick devices and sleep. |
| Trap (`TF`) | `1<<6` | Division/modulo by zero, an unrecoverable stack fault during interrupt dispatch | Nothing reads it today — it's informational, there's no `jt`/`jnt`. |

A crucial, deliberately non-obvious detail: **the Halting flag is *not* saved when an interrupt is
taken.** `triggerInterrupt()` masks it out of the flags word it pushes onto the stack before jumping
to the handler:

```cpp
const FlagRegister savedFlags{ _flags.value() & ~static_cast<FlagRegister::ValueType>(ExecutionFlag::Halting) };
```

`halt` means "wait for an interrupt"; if the halting bit were restored on `iret`, the machine would
go straight back to sleep the moment the handler returned, and nothing could ever wake it up again.

## Flag semantics per instruction group

- **`ADD`/`ADDI`/`ADDC`/`ADDCI`** (and their float form `FADD`): Zero/Sign reflect the 32-bit result;
  Carry is set on unsigned overflow (result `> 0xFFFFFFFF` in the 64-bit intermediate); Overflow uses
  the classic same-sign-operands/different-sign-result test for *signed* overflow.
- **`SUB`/`SUBI`/`SUBC`/`SUBCI`**: same shape, with Carry meaning "borrow occurred" (`a < b`).
- **`MUL`/`MULI` (unsigned)**: Carry and Overflow are both set together when the 64-bit product
  doesn't fit in 32 bits (there's no separate high/low result register — the high half is simply
  discarded).
- **`IMUL`/`IMULI` (signed)**: Carry and Overflow are both set together when the signed 64-bit product
  falls outside `[INT32_MIN, INT32_MAX]`.
- **`DIV`/`IDIV`/`MOD`/`IMOD`** (and their immediate forms): division/modulo **by zero does not
  throw or crash** — it sets the Trap flag, advances the program counter, and **leaves the
  destination register unchanged**. Signed division additionally sets Overflow only for the one
  case that can't be represented (`INT32_MIN / -1`).
- **Logic (`AND`/`OR`/`XOR`/`NOT`)**: Carry and Overflow are always cleared; only Zero/Sign are
  meaningful.
- **Shifts (`SHL`/`SHR`/`SAR`)**: the shift amount is masked to its low 5 bits (`b & 0x1F`), so
  shifting by 32 behaves like shifting by 0. A shift amount of exactly 0 leaves *all* flags
  untouched (not even Zero/Sign are recomputed) — this is a deliberate choice in
  `executeShl`/`executeShr`/`executeSar` to avoid `(a << -1)`-style undefined shift amounts. Carry
  takes the last bit shifted out.
- **`CMP`/`CMPI`/`FCMP`**: behaves exactly like a `SUB` for flag purposes, but discards the result
  (equivalent to x86's `cmp`). `FCMP` doesn't set Overflow (it "doesn't really make sense for
  floating-point comparisons", per the source comment) and doesn't set Carry from an unsigned
  perspective — it uses `a < b` on the floats directly.
- **Floating-point arithmetic (`FADD`/`FSUB`/`FMUL`/`FDIV`/`FNEG`)**: Zero reflects
  `std::fpclassify(result) == FP_ZERO`; Sign reflects `std::signbit(result)`; Carry is always
  `false`; Overflow is set only when the result becomes `±∞` and neither operand already was.

## Reading the flags after a `cmp`

`cmp` sets all four arithmetic flags, but no single-flag jump spells an **ordering**. The comparison
jumps read two flags each, and which pair depends on signedness:

| Ordering | Signed | Unsigned |
| --- | --- | --- |
| `<` | `Sign != Overflow` (`jls`) | `Carry` (`jbl`) |
| `>=` | `Sign == Overflow` (`jge`) | `!Carry` (`jae`) |
| `>` | `!Zero && Sign == Overflow` (`jgr`) | `!Carry && !Zero` (`jab`) |
| `<=` | `Zero \|\| Sign != Overflow` (`jle`) | `Carry \|\| Zero` (`jbe`) |

`FCMP` clears Overflow and puts `fs < ft` straight into Carry, so after a float comparison it is the
**unsigned** forms that read correctly. See
[Instruction set → Comparison jumps](05-Instruction-Set.md#comparison-jumps--0x680x77).

## Related pages

- [Instruction format](04-Instruction-Format.md) — where `rd`/`rs`/`rt`/`fd`/`fs`/`ft` sit in the 32-bit word.
- [Instruction set](05-Instruction-Set.md) — the full per-instruction reference.
- [Pseudo-instructions](06-Pseudo-Instructions.md) — why `at` gets clobbered.

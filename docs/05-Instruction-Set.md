# Instruction set

[← Back to index](README.md)

This page documents every **real** opcode the VM executes — the values of `ceres::vm::Opcode` in
[`opcodes.h`](../Ceres/libs/core/include/ceres/core/isa/opcodes.h). For the **pseudo-instructions** that expand into
several real ones (`la`, `ldv`, `stv`, `neg`, `swap`, the `ifXX` family and
the rest), see [Pseudo-instructions](06-Pseudo-Instructions.md).

## How to read these tables

- **Assembly** shows exactly what you write in a `.casm` file.
- **Opcode** is the encoded value in the top 8 bits of the instruction (see
  [Instruction format](04-Instruction-Format.md)).
- Where one mnemonic has several assembly forms (e.g. `add rd, rs, rt` vs. `add rd, rs, imm16`),
  **the assembler chooses the opcode automatically** based on the operand types — this is exactly
  what the table in [`instruction_info.cpp`](../Ceres/libs/asm/src/instruction_info.cpp)
  encodes. You never write the opcode name (`ADDI`) yourself; you always write the mnemonic (`add`)
  and the assembler picks the right variant.
- Mnemonics in `.casm` source are **case-insensitive** keywords, matched against `Mnemonic` in
  [`mnemonic.h`](../Ceres/libs/asm/include/ceres/asm/mnemonic.h) — `ADD`, `Add` and `add` are identical.
- "Flags" lists what each instruction updates; a dash means it does not touch the flags register at
  all.

## System control · `0x00`–`0x07`

| Assembly | Opcode | Operands | Semantics | Flags |
| --- | --- | --- | --- | --- |
| `nop` | `NOP` (`0x00`) | none | Does nothing; advances the program counter. | — |
| `halt` | `HALT` (`0x01`) | none | Sets the Halting flag. The `step()` loop then stops fetching instructions — it just ticks devices and sleeps 1 ms per iteration — until an interrupt arrives. | Halting |
| `trap` | `TRAP` (`0x02`) | none | Advances the PC, then raises interrupt `Trap` (1). Advancing first (rather than after) means `iret` returns to the instruction *after* the `trap`, not to the `trap` itself. | Trap-related interrupt is dispatched (see [Interrupts and exceptions](08-Interrupts-and-Exceptions.md)) |
| `reset` | `RESET` (`0x03`) | none | Raises interrupt `Reset` (0) *without* advancing the PC first. Because interrupt 0 is one of the always-deliverable "reserved" interrupts, this reliably restarts the machine regardless of the Interrupt flag. | — |
| `int imm8` | `INT` (`0x04`) | `imm8`: interrupt number 0–255 | Advances the PC, then raises the given interrupt number (cast to `InterruptNumber`). | — |
| `iret` | `IRET` (`0x05`) | none | Pops the saved PC and flags (pushed by the interrupt dispatcher, PC on top) and restores them, resuming exactly where the interrupt preempted execution. Does **not** advance the PC afterward — the popped value already points at the correct next instruction. | Fully restored from the stack (except Halting, see below) |
| `cli` | `CLI` (`0x06`) | none | Clears the Interrupt flag, masking interrupts 16–63. | Interrupt = 0 |
| `sti` | `STI` (`0x07`) | none | Sets the Interrupt flag, allowing interrupts 16–63 to be delivered. | Interrupt = 1 |

`iret` never restores the Halting bit even if it was set when the interrupt fired — see
[Registers and flags](03-Registers-and-Flags.md#flags-register) for why.

## Memory management unit · `0x08`–`0x0E`

| Assembly | Opcode | Operands | Semantics | Flags |
| --- | --- | --- | --- | --- |
| `mtp rs` | `MTP` (`0x08`) | `rs` | Page directory base register (PTBR) = `rs`. Also flushes the entire TLB. | — |
| `mfp rd` | `MFP` (`0x09`) | `rd` | `rd` = PTBR. Zero until the first `mtp`. | — |
| `pgon` | `PGON` (`0x0A`) | none | Sets the Paging flag: every subsequent load, store and instruction fetch is translated. | Paging = 1 |
| `pgoff` | `PGOFF` (`0x0B`) | none | Clears the Paging flag: addresses go straight to physical memory again. | Paging = 0 |
| `invlpg rs` | `INVLPG` (`0x0C`) | `rs` | Drops the TLB entry for the page containing the address in `rs`, if any. | — |
| `flpg` | `FLPG` (`0x0D`) | none | Drops every TLB entry. | — |
| `mfpf rd` | `MFPF` (`0x0E`) | `rd` | `rd` = the virtual address that last raised `PageFault` — CR2's equivalent. | — |

See [Virtual memory and paging](27-Virtual-Memory-and-Paging.md) for the two-level table format, the
TLB, which regions stay physical regardless of the Paging flag, and a worked example.

## Arithmetic · `0x10`–`0x28`

Every arithmetic mnemonic below has up to three encodings, chosen by operand shape:

- **Register form** — `mnemonic rd, rs, rt` — all-register, opcode has no suffix (`ADD`).
- **Immediate form** — `mnemonic rd, rs, imm16` — opcode suffixed `I` (`ADDI`). The immediate
  competes with `rt` for bits 15:0, so this form can never take a third register.
- **Float form** — `mnemonic fd, fs, ft` — chosen automatically when the operands are float
  registers; opcode prefixed `F` (`FADD`). Only exists where the operation makes sense on floats.

| Mnemonic | Reg. opcode | Imm. opcode | Float opcode | `rd = ...` | Flags |
| --- | --- | --- | --- | --- | --- |
| `add` | `ADD` `0x10` | `ADDI` `0x11` | `FADD` `0x14` | `rs + rt` (or `+ imm16`) | Zero, Sign, Carry (unsigned overflow), Overflow (signed overflow) |
| `adc` | `ADDC` `0x12` | `ADDCI` `0x13` | — | `rs + rt + carry` | Same as `add`, computed on the carry-adjusted operand |
| `sub` | `SUB` `0x15` | `SUBI` `0x16` | `FSUB` `0x19` | `rs - rt` (or `- imm16`) | Zero, Sign, Carry (borrow), Overflow |
| `sbc` | `SUBC` `0x17` | `SUBCI` `0x18` | — | `rs - rt - borrow` | Same as `sub` |
| `mul` | `MUL` `0x1A` | `MULI` `0x1B` | `FMUL` `0x1E` | `rs * rt` (unsigned) | Zero, Sign, Carry+Overflow (result doesn't fit in 32 bits) |
| `imul` | `IMUL` `0x1C` | `IMULI` `0x1D` | — | `rs * rt` (signed) | Zero, Sign, Carry+Overflow (result outside `i32` range) |
| `div` | `DIV` `0x1F` | `DIVI` `0x20` | `FDIV` `0x23` | `rs / rt` (unsigned) | Zero, Sign; **Trap instead of a crash on division by zero** (destination left unchanged) |
| `idiv` | `IDIV` `0x21` | `IDIVI` `0x22` | — | `rs / rt` (signed) | Zero, Sign, Overflow (only for `INT32_MIN / -1`); Trap on divide-by-zero |
| `mod` | `MOD` `0x24` | `MODI` `0x25` | — | `rs % rt` (unsigned) | Zero, Sign; Trap on divide-by-zero |
| `imod` | `IMOD` `0x26` | `IMODI` `0x27` | — | `rs % rt` (signed) | Zero, Sign; Trap on divide-by-zero |
| — | — | — | `FNEG` `0x28` | `-fs` | Zero, Sign, Overflow (`±∞` produced) |

`neg` (integer negation) is a **pseudo-instruction**, not a real opcode — see
[Pseudo-instructions](06-Pseudo-Instructions.md).

Divide-by-zero semantics (`div`, `idiv`, `mod`, `imod` and their immediate forms) are worth
repeating because they diverge from what most CPUs do: **the machine does not fault or halt.** It
sets the Trap flag, advances the PC, and leaves the destination register at whatever it already
held. Nothing currently reads the Trap flag automatically (see
[Registers and flags](03-Registers-and-Flags.md)), so a division by zero is silently survivable
unless the program itself checks for it beforehand.

## Logic and shifts · `0x30`–`0x3C`

| Mnemonic | Reg. opcode | Imm. opcode | `rd = ...` | Flags |
| --- | --- | --- | --- | --- |
| `and` | `AND` `0x30` | `ANDI` `0x31` | `rs & rt` | Zero, Sign; Carry/Overflow always cleared |
| `or` | `OR` `0x32` | `ORI` `0x33` | `rs \| rt` | Zero, Sign; Carry/Overflow always cleared |
| `xor` | `XOR` `0x34` | `XORI` `0x35` | `rs ^ rt` | Zero, Sign; Carry/Overflow always cleared |
| `not` | `NOT` `0x36` | — | `~rs` (unary; only `rd, rs`) | Zero, Sign; Carry/Overflow always cleared |
| `shl` | `SHL` `0x37` | `SHLI` `0x38` | `rs << (rt & 0x1F)` | Zero, Sign, Carry (last bit shifted out); unchanged if shift amount is 0 |
| `shr` | `SHR` `0x39` | `SHRI` `0x3A` | `rs >> (rt & 0x1F)` (logical) | Zero, Sign, Carry; unchanged if shift amount is 0 |
| `sar` | `SAR` `0x3B` | `SARI` `0x3C` | `(i32)rs >> (rt & 0x1F)` (arithmetic) | Zero, Sign, Carry; unchanged if shift amount is 0 |

Shift amounts are always masked to their low 5 bits (0–31), matching a 32-bit register width; a
shift amount of exactly zero is a documented no-op that leaves the flags untouched, not merely a
shift by an effective zero.

## Memory access · `0x40`–`0x4E`

Every displacement below is a **signed** 16-bit field: `-32768` to `32767`, measured from the base
register. A value outside that range is rejected at assembly time rather than truncated.


| Assembly | Opcode | Operands | Semantics |
| --- | --- | --- | --- |
| `mov rd, rs` | `MOV` `0x40` | 2 int regs | `rd = rs` |
| `mov fd, fs` | `FMOV` `0x41` | 2 float regs | `fd = fs` |
| `li rd, imm16` | `LI` `0x42` | reg + imm16 | `rd = imm16` (zero-extended) |
| `lui rd, imm16` | `LUI` `0x43` | reg + imm16 | `rd = imm16 << 16` |
| `ldr rd, [rs + imm16]` | `LDR` `0x48` | reg + reg + imm16 | `rd = *(u32*)(rs + simm16)`. Alignment-checked (4 bytes). |
| `ldrb rd, [rs + imm16]` | `LDRB` `0x44` | reg + reg + imm16 | `rd = *(u8*)(rs + simm16)`. Never faults on alignment. |
| `ldrh rd, [rs + imm16]` | `LDRH` `0x45` | reg + reg + imm16 | `rd = *(u16*)(rs + simm16)`. Alignment-checked (2 bytes). |
| `ldrsb rd, [rs + imm16]` | `LDRSB` `0x46` | reg + reg + imm16 | `rd = sign_extend(*(i8*)(rs + simm16))`. Never faults on alignment. |
| `ldrsh rd, [rs + imm16]` | `LDRSH` `0x47` | reg + reg + imm16 | `rd = sign_extend(*(i16*)(rs + simm16))`. Alignment-checked (2 bytes). |
| `ldr fd, [rs + imm16]` | `FLDR` `0x49` | float reg + int reg + imm16 | `fd = *(float*)(rs + simm16)`. Alignment-checked (4 bytes). |
| `str [rd + imm16], rs` | `STR` `0x4A` | reg + reg + imm16 | `*(u32*)(rd + simm16) = rs`. The destination comes first, like everywhere else; in the *encoding* the base is `rd` and the value `rs`, because `imm16` occupies `rt` — see [Instruction format](04-Instruction-Format.md#the-critical-overlap-imm16-and-rt). Alignment-checked. |
| `strb [rd + imm16], rs` | `STRB` `0x4B` | reg + reg + imm16 | `*(u8*)(rd + simm16) = rs`. Never faults on alignment. |
| `strh [rd + imm16], rs` | `STRH` `0x4C` | reg + reg + imm16 | `*(u16*)(rd + simm16) = rs`. Alignment-checked. |
| `str [rd + imm16], fs` | `FSTR` `0x4D` | reg + float reg + imm16 | `*(float*)(rd + simm16) = fs`. Alignment-checked. |
| `la rd, [rs + imm16]` | `LEA` `0x4E` | reg + reg + imm16 | `rd = rs + simm16` (computes the address, doesn't dereference it). `lea` is the old spelling. |

None of the memory instructions touch the flags register.

### Addressing syntax

```casm
ldr r1, [r2]              // no offset
ldr r1, [r2 + 4]           // literal offset
ldr r1, [r2 - 8]           // negative offset
ldr r1, [r2 - 8]           // negative: eight bytes *below* the base
ldr r1, [r2 + OFFSET]      // OFFSET must resolve to a constant
```

A negative displacement reaches below the base, which is what makes a frame pointer usable. It was
not always so: the field used to be zero-extended, so `[r2 - 8]` silently added 65528. Any `.cres`
written before that changed is rejected — see
[The CRES binary format](09-CRES-Binary-Format.md#versioning).

Whitespace around `+`/`-` inside brackets is mandatory: `[r5+0]` lexes as the register followed by
the *signed literal* `+0`, which the parser doesn't accept there — always write `[r5 + 0]`.

### Load/store variants at a glance

| Suffix | Width | Sign |
| --- | --- | --- |
| *(none)* (`ldr`/`str`) | 32-bit | n/a |
| `b` (`ldrb`/`strb`) | 8-bit | unsigned |
| `sb` (`ldrsb`) | 8-bit | sign-extended (load only — there's no narrowing 8-bit signed *store*, since `strb` already truncates) |
| `h` (`ldrh`/`strh`) | 16-bit | unsigned |
| `sh` (`ldrsh`) | 16-bit | sign-extended (load only) |

## Control flow · `0x50`–`0x77`

Every conditional/unconditional jump and `call` has two forms:

- **Relative** — `mnemonic label` or `mnemonic simm24` — displacement from the branch's own address,
  ±~8 MiB range. Chosen when the operand is a label or a 24-bit-fitting immediate.
- **Register** — `mnemonic rs` — absolute target taken from a register. Chosen automatically when the
  operand is a register instead of a label.

### Single-flag jumps

| Assembly | Relative opcode | Register opcode | Condition |
| --- | --- | --- | --- |
| `jp target` / `jmp target` | `JP` `0x50` | `JPR` `0x51` | Always |
| `jz target` / `jeq target` | `JZ` `0x55` | `JZR` `0x56` | Zero flag set |
| `jnz target` / `jne target` | `JNZ` `0x57` | `JNZR` `0x58` | Zero flag clear |
| `jc target` | `JC` `0x59` | `JCR` `0x5A` | Carry flag set |
| `jnc target` | `JNC` `0x5B` | `JNCR` `0x5C` | Carry flag clear |
| `js target` | `JS` `0x5D` | `JSR` `0x5E` | Sign flag set |
| `jns target` | `JNS` `0x5F` | `JNSR` `0x60` | Sign flag clear |
| `jo target` | `JO` `0x64` | `JOR` `0x65` | Overflow flag set |
| `jno target` | `JNO` `0x66` | `JNOR` `0x67` | Overflow flag clear |
| `call target` | `CALL` `0x61` | `CALLR` `0x62` | Always; pushes the return address first |

`jmp`, `jeq` and `jne` are aliases, not separate opcodes — the assembler emits `JP`, `JZ` and `JNZ`.

### Comparison jumps · `0x68`–`0x77`

`cmp` sets four flags, but none of the jumps above spells an **ordering**: "greater" needs Sign
against Overflow, and "above" needs Carry against Zero. These eight read two flags each, so a
comparison costs one instruction instead of a dance around the single-flag jumps.

| Assembly | Relative opcode | Register opcode | Condition | Flags read |
| --- | --- | --- | --- | --- |
| `jgr target` | `JGR` `0x68` | `JGRR` `0x69` | `rs > rt`, **signed** | `!Zero && Sign == Overflow` |
| `jge target` | `JGE` `0x6A` | `JGER` `0x6B` | `rs >= rt`, signed | `Sign == Overflow` |
| `jls target` | `JLS` `0x6C` | `JLSR` `0x6D` | `rs < rt`, signed | `Sign != Overflow` |
| `jle target` | `JLE` `0x6E` | `JLER` `0x6F` | `rs <= rt`, signed | `Zero \|\| Sign != Overflow` |
| `jab target` | `JAB` `0x70` | `JABR` `0x71` | `rs > rt`, **unsigned** | `!Carry && !Zero` |
| `jae target` | `JAE` `0x72` | `JAER` `0x73` | `rs >= rt`, unsigned | `!Carry` |
| `jbl target` | `JBL` `0x74` | `JBLR` `0x75` | `rs < rt`, unsigned | `Carry` |
| `jbe target` | `JBE` `0x76` | `JBER` `0x77` | `rs <= rt`, unsigned | `Carry \|\| Zero` |

The signed and unsigned halves genuinely differ. `0xFFFFFFFF` is `-1` as an `i32` and about four
billion as a `u32`, so against `1`:

```casm
    la  r1, 0xFFFFFFFF
    li  r2, 1
    cmp r1, r2
    jls .below      // taken:     -1 < 1
    jab .above      // also taken: 0xFFFFFFFF > 1
```

Pick by what the data means, not by which reads better. `jls`/`jge` for `i8`/`i16`/`i32`,
`jbl`/`jae` for `u8`/`u16`/`u32`, addresses and sizes.

Writing the comparison and the branch as one instruction is what the `ifXX` pseudo-instructions are
for — see [Pseudo-instructions](06-Pseudo-Instructions.md#comparison-and-branch-in-one-ifxx).

### Comparison and return

| Assembly | Opcode | Semantics |
| --- | --- | --- |
| `cmp rs, rt` | `CMP` `0x52` | Sets Zero/Sign/Carry/Overflow as if computing `rs - rt`, discarding the result. |
| `cmp rs, imm16` | `CMPI` `0x53` | Same, against a sign-extended 16-bit immediate. |
| `cmp fs, ft` | `FCMP` `0x54` | Float comparison; sets Zero/Sign, clears Overflow, uses `fs < ft` directly for Carry. |
| `ret` | `RET` `0x63` | Pops the return address pushed by `call`/`callr` and jumps to it. No operands. |

Because `FCMP` clears Overflow and sets Carry from `fs < ft`, the **unsigned** forms are the ones
that read a float comparison correctly: `jbl` after `cmp f0, f1` means `f0 < f1`. `ifXX` on a pair
of float registers picks `FCMP` and the matching branch for you, which is the point of it.

`call`/`callr` push the address of the instruction *following* the call (`pc + 4`) before jumping.
If the push would overflow the stack, the fault is raised and the jump never happens (see
[Memory](02-Memory.md#the-stack)).

## Indexed addressing · `0xB4`–`0xBD`

| Assembly | Opcode | Semantics |
| --- | --- | --- |
| `ldr rd, [rs + rt]` | `LDRX` `0xB4` | `rd = *(u32*)(rs + rt)` |
| `ldrb rd, [rs + rt]` | `LDRBX` `0xB5` | `rd = *(u8*)(rs + rt)` |
| `ldrh rd, [rs + rt]` | `LDRHX` `0xB6` | `rd = *(u16*)(rs + rt)` |
| `ldrsb rd, [rs + rt]` | `LDRSBX` `0xB7` | Sign-extending byte load |
| `ldrsh rd, [rs + rt]` | `LDRSHX` `0xB8` | Sign-extending halfword load |
| `ldr fd, [rs + rt]` | `FLDRX` `0xB9` | Float load |
| `str [rd + rt], rs` | `STRX` `0xBA` | `*(u32*)(rd + rt) = rs` |
| `strb [rd + rt], rs` | `STRBX` `0xBB` | `*(u8*)(rd + rt) = rs` |
| `strh [rd + rt], rs` | `STRHX` `0xBC` | `*(u16*)(rd + rt) = rs` |
| `str [rd + rt], fs` | `FSTRX` `0xBD` | Float store |

Walking an array used to cost an `add` per element, because the only offset a load could take was
a constant:

```casm
    add  r3, r8, r9         // and again, and again
    strb r2, [r3 + 0]
```

```casm
    strb r2, [r8 + r9]      // the same thing, in one instruction
```

The mnemonic does not change: `[rs + rt]` is an index and `[rs + 4]` is a displacement, and the
assembler picks the opcode from which one it sees — the same way `add` chooses between `ADD` and
`ADDI`. A [register alias](10-Language-Syntax.md#register-aliases) works as an index too.

Three things are deliberately not allowed:

- **`[r1 - r2]`**, because no opcode subtracts an index. Negate the index instead.
- **`[r1 + f2]`**, because a float register holds no address.
- **`[r1 + r2 + 4]`**, because `rt` and `imm16` are the same bits — see
  [Instruction format](04-Instruction-Format.md#the-critical-overlap-imm16-and-rt). An index and a
  displacement cannot both fit in one instruction.

The store form keeps the base in `Rd` and the value in `Rs`, like the displacement stores, because
`Rt` is now spoken for. Alignment is checked on the **sum**: two registers that are each aligned
can still add up to an address that is not.

## Branch and link · `0x78`–`0x79`

| Assembly | Opcode | Semantics |
| --- | --- | --- |
| `bl rd, label` | `BL` `0x78` | `rd = pc + 4; pc += simm20` |
| `bl rd, rs` | `BLR` `0x79` | `rd = pc + 4; pc = rs` |

`call` pushes the return address and `ret` pops it. That costs two memory accesses per call, which
a leaf function pays for nothing: it has no reason to touch the stack at all.

```casm
    bl r11, print_char      // r11 = the instruction after this one
    ...
print_char:
    outb 0x01, r0
    jp r11                  // the return: JPR, which already existed
```

The link register is an **operand**. That is the whole design: `bl` reserves nothing, so it does
not compete with `at` or with anything a convention wants, and `call`/`ret` keep working exactly
as they did for every function that wants the stack discipline. Adding a branch-and-link by
redefining `call` would have invalidated every existing `.cres` and forced a frame layout change;
adding one as a new opcode costs nothing anybody was already relying on.

**Reach.** `Rd` occupies bits 23:20, which a `simm24` displacement would otherwise use, so the
labelled form has twenty bits left: ±512 KiB, against `call`'s ±8 MiB. Further away is a link
error naming `call` as the way out. The register form has no such limit.

**Nothing saves the link for you.** A function that calls anything else overwrites its own return
address unless it puts it somewhere first — which is the bookkeeping `call`/`ret` does
automatically. `bl` is for leaves, and for tail calls, where a jump is the whole of the epilogue.

`bl rd, rs` reads its target before writing the link, so `bl r5, r5` is a well-defined call
through a pointer that keeps working.

## The rest of the arithmetic · `0x29`–`0x2F`, `0xCE`–`0xD1`

| Assembly | Opcode | Semantics |
| --- | --- | --- |
| `mulh rd, rs, rt` | `MULH` `0x29` | The high 32 bits of `rs * rt`, unsigned |
| `imulh rd, rs, rt` | `IMULH` `0x2A` | The high 32 bits, signed |
| `abs rd, rs` | `ABS` `0x2B` | `\|rs\|` as a signed value |
| `min` / `max` | `0x2C`, `0x2E` | Unsigned, register or `imm16` (`MINI` `0xCE`, `MAXI` `0xD0`) |
| `imin` / `imax` | `0x2D`, `0x2F` | Signed, register or `simm16` (`IMINI` `0xCF`, `IMAXI` `0xD1`) |
| `sqrt fd, fs` | `FSQRT` `0x96` | Square root; no integer form |
| `abs fd, fs` | `FABS` `0x97` | Absolute value, chosen by the register bank like `neg` |

`mul` keeps the low 32 bits of a 32×32 product and drops the rest, which means an overflow left no
trace at all. `mulh` is that missing half: the two together are the full 64-bit product.

`abs` has one value it cannot answer for. `|INT_MIN|` is 2147483648, which is not an `i32`, so the
result is `INT_MIN` again and **Overflow** is set — the machine says it could not rather than
returning a negative absolute value in silence.

All of these set Zero and Sign from the result and clear Carry and Overflow, the way the bitwise
operations do. `abs` is the exception, and sets its own Overflow.

## Extended float arithmetic · `0x98`–`0x9F`, `0xD2`–`0xD6`

The four basic float operations, `fneg`, `fsqrt`, `fabs` and `fcmp` cover arithmetic but not the
primitives a software math library needs to build `sin`, `log`, `exp` or `pow` on top of them:
remainder for range reduction, an explicit rounding mode, sign injection, and a fused
multiply-accumulate for evaluating a polynomial without paying for intermediate rounding twice.
Nothing here is a transcendental itself: `sin`/`cos`/`log`/`exp`/`pow` stay software, built on top
of these — the same line x87 (SSE/AVX and later), ARM, RISC-V and WebAssembly all draw, and for the
same reason. A future `stdlib/math.casm` is the intended home for them.

| Assembly | Opcode | Operands | Semantics | Flags |
| --- | --- | --- | --- | --- |
| `fmod fd, fs, ft` / `mod fd, fs, ft` | `FMOD` `0x98` | 3 float regs | `fd = fmod(fs, ft)`, the IEEE remainder | Zero, Sign; **Trap on `ft == 0`**, like `fdiv` |
| `fmin fd, fs, ft` / `min fd, fs, ft` | `FMIN` `0x99` | 3 float regs | `fd = min(fs, ft)` | Zero, Sign; Carry/Overflow always cleared |
| `fmax fd, fs, ft` / `max fd, fs, ft` | `FMAX` `0x9A` | 3 float regs | `fd = max(fs, ft)` | Zero, Sign; Carry/Overflow always cleared |
| `fround fd, fs` | `FROUND` `0x9B` | 2 float regs | `fd = fs` rounded to the nearest integer, ties to even | — |
| `ffloor fd, fs` | `FFLOOR` `0x9C` | 2 float regs | `fd = ⌊fs⌋` | — |
| `fceil fd, fs` | `FCEIL` `0x9D` | 2 float regs | `fd = ⌈fs⌉` | — |
| `ftrunc fd, fs` | `FTRUNC` `0x9E` | 2 float regs | `fd = fs` truncated toward zero | — |
| `fcopysign fd, fs, ft` | `FCOPYSIGN` `0x9F` | 3 float regs | `fd = \|fs\|` with the sign of `ft` | — |
| `fma fd, fs, ft` | `FMA` `0xD2` | 3 float regs | `fd = fd + fs * ft` — `fd` is read as the accumulator as well as written | Same as `fadd`, applied to `(fd, fs*ft)` |
| `fclass rd, fs` | `FCLASS` `0xD3` | int reg + float reg | `rd` = a one-hot bitmask classifying `fs` (see below) | — |
| `frecipe fd, fs` | `FRECIPE` `0xD4` | 2 float regs | `fd = 1 / fs` | — (**Trap on `fs == 0`**, like `fdiv`) |
| `frsqrte fd, fs` | `FRSQRTE` `0xD5` | 2 float regs | `fd = 1 / sqrt(fs)` | — (**Trap on `fs == 0`**, like `fdiv`) |

`mod`/`min`/`max` pick `FMOD`/`FMIN`/`FMAX` for a pair of float registers the same way `add`/`sub`/
`mul`/`div`/`abs`/`neg` already pick their `F`-prefixed opcode — `fmod`/`fmin`/`fmax` are the
explicit spellings, `mod`/`min`/`max` the consistent ones. The other eight extended-float opcodes
have no integer sibling to share a mnemonic with, so each gets one mnemonic of its own.

`fround`/`ffloor`/`fceil`/`ftrunc` and `frecipe`/`frsqrte` touch no flags at all, the same as
`fsqrt`/`fabs`: each hands back an unambiguous float and there is nothing a flag would add.
`fmin`/`fmax`/`fcopysign` set Zero/Sign from the result and always clear Carry/Overflow, the same
way the bitwise operations do — there is no carry or overflow to report from picking or
reassembling a value that already existed. `fma` reuses `fadd`'s exact flag and rounding behaviour,
because it *is* an `fadd` once the product `fs * ft` has been computed.

**`frecipe`/`frsqrte` are estimates in name only.** Real hardware reciprocal-estimate instructions
(ARM's `FRECPE`/`FRSQRTE`, x86's `rcpss`/`rsqrtss`) trade precision for speed, refined afterward
with a Newton-Raphson step in software — worthwhile because a full division is expensive in
silicon. Ceres is a software-interpreted VM: a division costs the interpreter one C++ operator,
exactly like any other float op, so there is no speed to buy by answering approximately. Both give
an exact result instead, and keep the name only as the hardware-precedented spelling of "you were
about to write `li 1.0` then `fdiv`; here is one instruction for that."

**`fclass`'s bitmask**, RISC-V `fclass`-inspired but collapsed to a single NaN bit rather than
separating quiet from signaling (the standard library does not portably distinguish them):

| Bit | Meaning | Bit | Meaning |
| --- | --- | --- | --- |
| 0 | `-Infinity` | 5 | `+Subnormal` |
| 1 | `-Normal` | 6 | `+Normal` |
| 2 | `-Subnormal` | 7 | `+Infinity` |
| 3 | `-0` | 8 | `NaN` |
| 4 | `+0` | | |

Exactly one bit is ever set. `log`/`sqrt` of a negative number, or any other domain error, shows up
as the NaN bit without a comparison against zero and sign for every caller to repeat.

`fmod`, like `fdiv`, chooses to trap on a zero divisor rather than follow IEEE 754 (which defines
`fmod(x, 0)` as NaN) — consistency with the rest of the divide family wins over strict IEEE
conformance, exactly as documented for `fdiv` above. `frecipe`/`frsqrte` extend the same rule to
their own zero divisor. Negative input to `frsqrte` is *not* specially handled — `sqrt` of a
negative is already silently NaN via `fsqrt`, and `1 / NaN` stays NaN, so nothing new needed
guarding.

## Bits · `0x3D`–`0x3F`, `0xC8`–`0xCD`

| Assembly | Opcode | Semantics |
| --- | --- | --- |
| `clz rd, rs` | `CLZ` `0x3D` | Leading zero bits; 32 when `rs` is zero |
| `popcnt rd, rs` | `POPCNT` `0x3E` | Set bits |
| `bswap rd, rs` | `BSWAP` `0x3F` | The four bytes reversed |
| `rol rd, rs, rt` | `ROL` `0xC8` | Rotate left (`ROLI` `0xC9` for an immediate) |
| `ror rd, rs, rt` | `ROR` `0xCA` | Rotate right (`RORI` `0xCB`) |
| `sxtb rd, rs` | `SXTB` `0xCC` | The low byte, sign-extended |
| `sxth rd, rs` | `SXTH` `0xCD` | The low halfword, sign-extended |

Rotate amounts use the low five bits, like the shifts, so a rotation by 32 is a rotation by none.

`sxtb` and `sxth` do for a register what `ldrsb` and `ldrsh` do for memory. A value that arrived in
a register some other way — from a port, from an unpacked field — had to be widened with a pair of
shifts, which is two instructions and a magic number for something the machine already knows how
to do.

## Stack operations · `0x80`–`0x89`

| Assembly | Opcode | Semantics |
| --- | --- | --- |
| `push rs` | `PUSH` `0x80` | `*(u32*)(--sp) = rs`. |
| `pop rd` | `POP` `0x81` | `rd = *(u32*)(sp); sp += 4`. |
| `push` | `PUSHF` `0x82` | With no operand: pushes the full flags register. `pushf` spells the same thing. |
| `pop` | `POPF` `0x83` | With no operand: pops into the flags register, overwriting all flags at once. |
| `push fs` | `FPUSH` `0x84` | Pushes a float register (4 bytes, bit pattern preserved). |
| `pop fd` | `FPOP` `0x85` | Pops into a float register. |
| `pushm imm16` | `PUSHM` `0x86` | Pushes every register whose bit is set, `r15` first. |
| `popm imm16` | `POPM` `0x87` | Pops into every register whose bit is set, `r0` first. |
| `enter imm16` | `ENTER` `0x88` | `push fp; fp = sp; sp -= imm16` — a whole prologue. |
| `leave` | `LEAVE` `0x89` | `sp = fp; pop fp` — and the whole epilogue. |

### `enter` and `leave`

These are the only instructions that touch `fp`. A frame used to cost three instructions and
twelve bytes at the top of every function that had one:

```casm
    push fp
    mov  fp, sp
    sub  sp, sp, Frame
```

```casm
    enter Frame         // the same thing, in one word
```

The operand is an ordinary 16-bit immediate, so a [`struct`](23-Structs.md) name works directly —
the struct's own name is its size. A bare `enter` opens a frame of nothing, which still saves `fp`
so that `leave` has something to restore; that is the shape for a function whose locals all live in
registers but which still wants a frame pointer.

`enter` checks room for the saved `fp` **and** the frame before it writes anything. A prologue that
saved `fp` and then found no room for the frame would leave the function running on a stack it does
not own, with an epilogue that would restore from the wrong place.

### `pushm` and `popm`

Bit *n* of the immediate means register *n*, so `0x0F00` is `r8`–`r11` and `0x0003` is `r0` and
`r1`. The immediate field is sixteen bits wide and the bank is sixteen registers deep, which is
the whole reason the mask fits.

```casm
my_function:
    pushm 0x0F00            // save r8-r11 in one word instead of four stores
    ...
    popm  0x0F00
    ret
```

The store order is fixed: `pushm` goes from the highest set bit down, so the lowest-numbered
register ends up at the lowest address, and `popm` reads back up from `r0`. A matching pair
therefore restores exactly what it saved for any mask, and the saved area can be described by a
[`struct`](23-Structs.md) whose fields run in register order.

`pushm` is **all or nothing**. It checks room for the whole mask before storing anything, and
raises `StackOverflow` without touching the stack if it does not fit — a partially saved frame
would be restored as though it were whole, which is worse than not starting. `popm` checks the
same way against the data actually on the stack.

There is no register-list syntax: the mask is written as a number, and the constant expression
grammar has `+` but no `|`, so disjoint bits add up the way you would expect — `pushm 256 + 512`
is `r8` and `r9`.

Every push/pop can raise `StackOverflow` — see [Memory](02-Memory.md#the-stack). When that happens,
the instruction that triggered it does not complete (e.g. `push` does not advance the PC if the push
faulted), because the fault handler has already redirected execution to the handler.

## Conversions · `0x90`–`0x95`

| Assembly | Opcode | Semantics |
| --- | --- | --- |
| `itof fd, rs` | `ITOF` `0x90` | `fd = (float)(u32)rs` — treats `rs` as unsigned. |
| `iitof fd, rs` | `IITOF` `0x91` | `fd = (float)(i32)rs` — treats `rs` as signed. |
| `ftoi rd, fs` | `FTOI` `0x92` | `rd = (u32)(float)fs` — truncates toward zero, unsigned result. |
| `ftoii rd, fs` | `FTOII` `0x93` | `rd = (u32)(i32)(float)fs` — truncates toward zero, signed result stored in the register bits. |
| `mtf fd, rs` | `MTF` `0x94` | Copies the raw 32-bit *bit pattern* of `rs` into `fd`. No numeric conversion. |
| `mff rd, fs` | `MFF` `0x95` | Copies the raw 32-bit *bit pattern* of `fs` into `rd`. No numeric conversion. |

None of the conversions touch the flags register.

## I/O operations · `0xA0`–`0xB3`

Every I/O mnemonic (`in`, `inb`, `inh`, `insb`, `insh`, `inm`, `out`, `outb`, `outh`, `outm`) has two
forms depending on how the **port number** is specified — the assembler picks between them
automatically:

- **Immediate port** — `mnemonic imm8, ...` — the port is a fixed 8-bit literal, encoded straight
  into the instruction's `imm8` field.
- **Register port** — `mnemonic reg, ...` — the port number is read at run time from a register
  (opcode suffixed `R`, e.g. `INR` for `IN` with a register port).

| Assembly | Imm. port opcode | Reg. port opcode | Semantics |
| --- | --- | --- | --- |
| `in rd, port` | `IN` `0xA0` | `INR` `0xA6` | `rd = (u32)` word read from `port`. |
| `inb rd, port` | `INB` `0xA1` | `INRB` `0xA7` | `rd = (u8)` byte read from `port` (zero-extended). |
| `inh rd, port` | `INH` `0xA2` | `INRH` `0xA8` | `rd = (u16)` halfword read (zero-extended). |
| `insb rd, port` | `INSB` `0xA3` | `INRSB` `0xA9` | `rd = sign_extend((i8)` byte read`)`. |
| `insh rd, port` | `INSH` `0xA4` | `INRSH` `0xAA` | `rd = sign_extend((i16)` halfword read`)`. |
| `inm rd, port, rs` | `INM` `0xA5` | `INRM` `0xAB` | Block read: reads `rs` bytes from `port` into memory starting at address `rd`. Operand order in assembly is **port, address, size**. |
| `out port, rs` | `OUT` `0xAC` | `OUTR` `0xB0` | Writes the 32-bit value in `rs` to `port`. |
| `outb port, rs` | `OUTB` `0xAD` | `OUTRB` `0xB1` | Writes the low byte of `rs` to `port`. |
| `outh port, rs` | `OUTH` `0xAE` | `OUTRH` `0xB2` | Writes the low halfword of `rs` to `port`. |
| `outm port, rs, rt` | `OUTM` `0xAF` | `OUTRM` `0xB3` | Block write: writes `rt` bytes from memory address `rs` to `port`. Operand order is **port, address, size**, matching `inm`. |

### The opcodes moved

Adding the comparison jumps left only eight free slots where sixteen were needed, so the control-flow
block grew to `0x77` and pushed the three families above it up: stack `0x70`→`0x80`, conversions
`0x80`→`0x90`, I/O `0x90`→`0xA0`. Every family starts on a round boundary again and keeps a gap to
grow into. Free today: `0x08`–`0x0F`, `0x29`–`0x2F`, `0x3D`–`0x3F`, `0x4F`, `0x78`–`0x7F`,
`0x86`–`0x8F`, `0x96`–`0x9F`, `0xB4`–`0xFF`.

That is a **binary-breaking** change: a `.cres` written before it has `0x70` meaning `PUSH` where it
now means `JAB`. The file format's version was raised to 2 and a minimum supported version added, so
an older file is rejected rather than run as something else — see
[The CRES binary format](09-CRES-Binary-Format.md#versioning).

None of the I/O instructions touch the flags register. A read from an unattached port returns all
ones (`0xFF`, `0xFFFF`, `0xFFFFFFFF` depending on width); a write to an unattached port is silently
discarded. See [I/O devices and ports](07-IO-Devices-and-Ports.md) for the concrete port map and
which of these are actually backed by a device today.

## Related pages

- [Instruction format](04-Instruction-Format.md) — bit-level encoding these tables build on.
- [Pseudo-instructions](06-Pseudo-Instructions.md) — `la`, `ldv`, `stv`, `neg`, `ifXX` and the rest.
- [I/O devices and ports](07-IO-Devices-and-Ports.md) — what's actually listening on each port.
- [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — `int`/`iret` and hardware-raised faults.
- [Virtual memory and paging](27-Virtual-Memory-and-Paging.md) — `mtp`/`mfp`/`pgon`/`pgoff`/`invlpg`/`flpg`/`mfpf` in depth.

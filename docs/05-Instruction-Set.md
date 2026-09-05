# Instruction set

[← Back to index](README.md)

This page documents every **real** opcode the VM executes — the values of `ceres::vm::Opcode` in
[`opcodes.h`](../Ceres-ASM/src/vm/opcodes.h). For the four **pseudo-instructions** that expand into
several real ones (`la`, `ldv`, `stv`, `neg`), see [Pseudo-instructions](06-Pseudo-Instructions.md).

## How to read these tables

- **Assembly** shows exactly what you write in a `.casm` file.
- **Opcode** is the encoded value in the top 8 bits of the instruction (see
  [Instruction format](04-Instruction-Format.md)).
- Where one mnemonic has several assembly forms (e.g. `add rd, rs, rt` vs. `add rd, rs, imm16`),
  **the assembler chooses the opcode automatically** based on the operand types — this is exactly
  what the table in [`instruction_info.cpp`](../Ceres-ASM/src/assembler/instruction_info.cpp)
  encodes. You never write the opcode name (`ADDI`) yourself; you always write the mnemonic (`add`)
  and the assembler picks the right variant.
- Mnemonics in `.casm` source are **case-insensitive** keywords, matched against `Mnemonic` in
  [`mnemonic.h`](../Ceres-ASM/src/assembler/mnemonic.h) — `ADD`, `Add` and `add` are identical.
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

| Assembly | Opcode | Operands | Semantics |
| --- | --- | --- | --- |
| `mov rd, rs` | `MOV` `0x40` | 2 int regs | `rd = rs` |
| `mov fd, fs` | `FMOV` `0x41` | 2 float regs | `fd = fs` |
| `li rd, imm16` | `LI` `0x42` | reg + imm16 | `rd = imm16` (zero-extended) |
| `lui rd, imm16` | `LUI` `0x43` | reg + imm16 | `rd = imm16 << 16` |
| `ldr rd, [rs + imm16]` | `LDR` `0x48` | reg + reg + imm16 | `rd = *(u32*)(rs + imm16)`. Alignment-checked (4 bytes). |
| `ldrb rd, [rs + imm16]` | `LDRB` `0x44` | reg + reg + imm16 | `rd = *(u8*)(rs + imm16)`. Never faults on alignment. |
| `ldrh rd, [rs + imm16]` | `LDRH` `0x45` | reg + reg + imm16 | `rd = *(u16*)(rs + imm16)`. Alignment-checked (2 bytes). |
| `ldrsb rd, [rs + imm16]` | `LDRSB` `0x46` | reg + reg + imm16 | `rd = sign_extend(*(i8*)(rs + imm16))`. Never faults on alignment. |
| `ldrsh rd, [rs + imm16]` | `LDRSH` `0x47` | reg + reg + imm16 | `rd = sign_extend(*(i16*)(rs + imm16))`. Alignment-checked (2 bytes). |
| `ldr fd, [rs + imm16]` | `FLDR` `0x49` | float reg + int reg + imm16 | `fd = *(float*)(rs + imm16)`. Alignment-checked (4 bytes). |
| `str [rd + imm16], rs` | `STR` `0x4A` | reg + reg + imm16 | `*(u32*)(rd + imm16) = rs`. **Base is `rd`, value is `rs`** — see [Instruction format](04-Instruction-Format.md#the-critical-overlap-imm16-and-rt) for why. Alignment-checked. |
| `strb [rd + imm16], rs` | `STRB` `0x4B` | reg + reg + imm16 | `*(u8*)(rd + imm16) = rs`. Never faults on alignment. |
| `strh [rd + imm16], rs` | `STRH` `0x4C` | reg + reg + imm16 | `*(u16*)(rd + imm16) = rs`. Alignment-checked. |
| `str [rd + imm16], fs` | `FSTR` `0x4D` | reg + float reg + imm16 | `*(float*)(rd + imm16) = fs`. Alignment-checked. |
| `lea rd, [rs + imm16]` | `LEA` `0x4E` | reg + reg + imm16 | `rd = rs + imm16` (computes the address, doesn't dereference it). |

None of the memory instructions touch the flags register.

### Addressing syntax

```casm
ldr r1, [r2]              // no offset
ldr r1, [r2 + 4]           // literal offset
ldr r1, [r2 - 8]           // negative offset
ldr r1, [r2 + OFFSET]      // OFFSET must resolve to a constant
```

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

## Control flow · `0x50`–`0x67`

Every conditional/unconditional jump and `call` has two forms:

- **Relative** — `mnemonic label` or `mnemonic simm24` — displacement from the branch's own address,
  ±~8 MiB range. Chosen when the operand is a label or a 24-bit-fitting immediate.
- **Register** — `mnemonic rs` — absolute target taken from a register. Chosen automatically when the
  operand is a register instead of a label.

| Assembly | Relative opcode | Register opcode | Condition |
| --- | --- | --- | --- |
| `jp target` | `JP` `0x50` | `JPR` `0x51` | Always |
| `jz target` | `JZ` `0x55` | `JZR` `0x56` | Zero flag set |
| `jnz target` | `JNZ` `0x57` | `JNZR` `0x58` | Zero flag clear |
| `jc target` | `JC` `0x59` | `JCR` `0x5A` | Carry flag set |
| `jnc target` | `JNC` `0x5B` | `JNCR` `0x5C` | Carry flag clear |
| `js target` | `JS` `0x5D` | `JSR` `0x5E` | Sign flag set |
| `jns target` | `JNS` `0x5F` | `JNSR` `0x60` | Sign flag clear |
| `jo target` | `JO` `0x64` | `JOR` `0x65` | Overflow flag set |
| `jno target` | `JNO` `0x66` | `JNOR` `0x67` | Overflow flag clear |
| `call target` | `CALL` `0x61` | `CALLR` `0x62` | Always; pushes the return address first |

| Assembly | Opcode | Semantics |
| --- | --- | --- |
| `cmp rs, rt` | `CMP` `0x52` | Sets Zero/Sign/Carry/Overflow as if computing `rs - rt`, discarding the result. |
| `cmp rs, imm16` | `CMPI` `0x53` | Same, against a sign-extended 16-bit immediate. |
| `cmp fs, ft` | `FCMP` `0x54` | Float comparison; sets Zero/Sign, clears Overflow, uses `fs < ft` directly for Carry. |
| `ret` | `RET` `0x63` | Pops the return address pushed by `call`/`callr` and jumps to it. No operands. |

`call`/`callr` push the address of the instruction *following* the call (`pc + 4`) before jumping.
If the push would overflow the stack, the fault is raised and the jump never happens (see
[Memory](02-Memory.md#the-stack)).

## Stack operations · `0x70`–`0x75`

| Assembly | Opcode | Semantics |
| --- | --- | --- |
| `push rs` | `PUSH` `0x70` | `*(u32*)(--sp) = rs`. |
| `pop rd` | `POP` `0x71` | `rd = *(u32*)(sp); sp += 4`. |
| `pushf` | `PUSHF` `0x72` | Pushes the full flags register. |
| `popf` | `POPF` `0x73` | Pops into the flags register (overwrites all flags at once). |
| `push fs` | `FPUSH` `0x74` | Pushes a float register (4 bytes, bit pattern preserved). |
| `pop fd` | `FPOP` `0x75` | Pops into a float register. |

Every push/pop can raise `StackOverflow` — see [Memory](02-Memory.md#the-stack). When that happens,
the instruction that triggered it does not complete (e.g. `push` does not advance the PC if the push
faulted), because the fault handler has already redirected execution to the handler.

## Conversions · `0x80`–`0x85`

| Assembly | Opcode | Semantics |
| --- | --- | --- |
| `itof fd, rs` | `ITOF` `0x80` | `fd = (float)(u32)rs` — treats `rs` as unsigned. |
| `iitof fd, rs` | `IITOF` `0x81` | `fd = (float)(i32)rs` — treats `rs` as signed. |
| `ftoi rd, fs` | `FTOI` `0x82` | `rd = (u32)(float)fs` — truncates toward zero, unsigned result. |
| `ftoii rd, fs` | `FTOII` `0x83` | `rd = (u32)(i32)(float)fs` — truncates toward zero, signed result stored in the register bits. |
| `mtf fd, rs` | `MTF` `0x84` | Copies the raw 32-bit *bit pattern* of `rs` into `fd`. No numeric conversion. |
| `mff rd, fs` | `MFF` `0x85` | Copies the raw 32-bit *bit pattern* of `fs` into `rd`. No numeric conversion. |

None of the conversions touch the flags register.

## I/O operations · `0x90`–`0xA3`

Every I/O mnemonic (`in`, `inb`, `inh`, `insb`, `insh`, `inm`, `out`, `outb`, `outh`, `outm`) has two
forms depending on how the **port number** is specified — the assembler picks between them
automatically:

- **Immediate port** — `mnemonic imm8, ...` — the port is a fixed 8-bit literal, encoded straight
  into the instruction's `imm8` field.
- **Register port** — `mnemonic reg, ...` — the port number is read at run time from a register
  (opcode suffixed `R`, e.g. `INR` for `IN` with a register port).

| Assembly | Imm. port opcode | Reg. port opcode | Semantics |
| --- | --- | --- | --- |
| `in rd, port` | `IN` `0x90` | `INR` `0x96` | `rd = (u32)` word read from `port`. |
| `inb rd, port` | `INB` `0x91` | `INRB` `0x97` | `rd = (u8)` byte read from `port` (zero-extended). |
| `inh rd, port` | `INH` `0x92` | `INRH` `0x98` | `rd = (u16)` halfword read (zero-extended). |
| `insb rd, port` | `INSB` `0x93` | `INRSB` `0x99` | `rd = sign_extend((i8)` byte read`)`. |
| `insh rd, port` | `INSH` `0x94` | `INRSH` `0x9A` | `rd = sign_extend((i16)` halfword read`)`. |
| `inm port, rd, rs` | `INM` `0x95` | `INRM` `0x9B` | Block read: reads `rs` bytes from `port` into memory starting at address `rd`. Operand order in assembly is **port, address, size**. |
| `out port, rs` | `OUT` `0x9C` | `OUTR` `0xA0` | Writes the 32-bit value in `rs` to `port`. |
| `outb port, rs` | `OUTB` `0x9D` | `OUTRB` `0xA1` | Writes the low byte of `rs` to `port`. |
| `outh port, rs` | `OUTH` `0x9E` | `OUTRH` `0xA2` | Writes the low halfword of `rs` to `port`. |
| `outm port, rs, rt` | `OUTM` `0x9F` | `OUTRM` `0xA3` | Block write: writes `rt` bytes from memory address `rs` to `port`. Operand order is **port, address, size**, matching `inm`. |

None of the I/O instructions touch the flags register. A read from an unattached port returns all
ones (`0xFF`, `0xFFFF`, `0xFFFFFFFF` depending on width); a write to an unattached port is silently
discarded. See [I/O devices and ports](07-IO-Devices-and-Ports.md) for the concrete port map and
which of these are actually backed by a device today.

## Related pages

- [Instruction format](04-Instruction-Format.md) — bit-level encoding these tables build on.
- [Pseudo-instructions](06-Pseudo-Instructions.md) — `la`, `ldv`, `stv`, `neg`.
- [I/O devices and ports](07-IO-Devices-and-Ports.md) — what's actually listening on each port.
- [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — `int`/`iret` and hardware-raised faults.

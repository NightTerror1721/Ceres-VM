# Instruction format

[← Back to index](README.md)

Every Ceres instruction is exactly **32 bits**, stored little-endian in memory. There is a single
fixed layout; different opcodes simply interpret different subsets of the bits.

```
 31      24 23    20 19    16 15    12 11                    0
+----------+--------+--------+--------+-----------------------+
|  opcode  | rd/fd  | rs/fs  | rt/ft  |                       |
+----------+--------+--------+--------+-----------------------+
                             |<------------ imm16 ----------->|
           |<------------------- imm24 ---------------------->|
                                              |<--- imm8 ----->|
```

From [`instructions.h`](../Ceres/libs/core/include/ceres/core/isa/instructions.h):

| Field | Bits | Mask | Notes |
| --- | --- | --- | --- |
| `opcode` | 31:24 | `0xFF000000` | One of the 8-bit values in [`opcodes.h`](../Ceres/libs/core/include/ceres/core/isa/opcodes.h). |
| `rd` / `fd` | 23:20 | `0x00F00000` | Destination register. Only 4 bits — a register index is always 0–15. |
| `rs` / `fs` | 19:16 | `0x000F0000` | First source register. |
| `rt` / `ft` | 15:12 | `0x0000F000` | Second source register. |
| `imm16` / `simm16` | 15:0 | `0x0000FFFF` | 16-bit immediate, unsigned or sign-extended depending on the accessor used. |
| `imm24` / `simm24` | 23:0 | `0x00FFFFFF` | 24-bit immediate, used for relative branch displacements. |
| `imm8` | 7:0 | `0x000000FF` | 8-bit immediate, used for I/O port numbers and `INT`. |

`fd`/`fs`/`ft` are not separate bit positions — they are just `rd`/`rs`/`rt` read under a different
name when the opcode is a floating-point one (`Instruction::fd()` literally returns the same bits as
`rd()`). This is why an assembly-level float register like `f7` and an integer register `r7` compile
to the identical 4-bit pattern `0111`; only the opcode tells them apart.

## The critical overlap: `imm16` and `rt`

**`imm16` occupies bits 15:0, which is exactly where `rt` lives.** An instruction cannot use both a
third register operand *and* a 16-bit immediate at the same time — this is a hard limit of the
format, not an arbitrary rule. It's the direct reason for a few syntax choices that otherwise look
backwards:

- **Stores put the base address in `rd` and the value in `rs`**, freeing up the `rt`/`imm16` slot
  for the displacement:

  ```casm
  str [rd + imm16], rs
  ```

  If stores instead put the value in `rd` and the base in `rs` (as loads do), there would be no room
  left to also encode a 16-bit offset.

- **There is no way to load a full 32-bit immediate in a single instruction.** The widest immediate
  field is 16 bits, so a full address requires two instructions: `lui` (Load Upper Immediate) sets
  the high 16 bits, and `ori` sets the low 16 bits. This two-instruction sequence is exactly what the
  `la`, `ldv` and `stv` pseudo-instructions expand to — see
  [Pseudo-instructions](06-Pseudo-Instructions.md).

## Relative branches

Conditional and unconditional jumps (`jp`, `jz`, `call`, …) that take a label or a signed 24-bit
immediate use the `imm24`/`simm24` field as a **relative** displacement, measured from the branch
instruction's own address, giving a reach of roughly ±8 MiB (`i24`'s range). `Instruction::simm24()`
sign-extends the 24-bit field before use:

```cpp
_pc += inst.simm24().signedValue();
```

The *register* forms of the same branches (`jpr`, `jzr`, `callr`, …) instead take the *absolute*
target address straight out of a register — no displacement arithmetic involved.

## Reading and writing fields

[`instructions.h`](../Ceres/libs/core/include/ceres/core/isa/instructions.h) exposes both accessors and constructors for
every field:

```cpp
Opcode opcode() const noexcept;
u24  imm24()  const noexcept;  i24  simm24() const noexcept;
u16  imm16()  const noexcept;  i16  simm16() const noexcept;
u8   imm8()   const noexcept;
u8   rd()/rs()/rt()/fd()/fs()/ft() const noexcept;

static Instruction make(Opcode opcode);                                   // no operands
static Instruction make(Opcode opcode, u8 imm8);                          // INT-style
static Instruction make(Opcode opcode, u24/i24 imm24);                    // JP-style
static Instruction make(Opcode opcode, u8 rd, u8 rs, u16 imm16 = 0);      // ADDI-style
static Instruction make(Opcode opcode, u8 rd, u8 rs, u8 rt, u8 imm8 = 0); // ADD-style
```

Plus one named factory per instruction (`Instruction::ADD(rd, rs, rt)`,
`Instruction::LDR(rd, rs, imm16)`, …), which the assembler's `BinaryEmitter` and the VM's own test
suite both use instead of calling `make()` directly.

## Signed and unsigned immediate fields

The same sixteen bits are read differently depending on the instruction:

| Read as | Used by |
| --- | --- |
| **Unsigned** | `li`, `lui`, and the ALU immediate forms (`addi`, `andi`, `shli`, …) |
| **Signed** | Every memory displacement (`ldr`/`str`/`la` and their widths), `cmpi`, and the signed ALU forms (`imuli`, `idivi`, `imodi`) |

The distinction matters most for a displacement: `[fp - 8]` has to reach below the base, not 65528
bytes above it, which is what a zero-extended field would do. The assembler rejects a displacement
outside `-32768`–`32767` rather than truncating it.

## Related pages

- [Registers and flags](03-Registers-and-Flags.md) — what `rd`/`rs`/`rt` refer to.
- [Instruction set](05-Instruction-Set.md) — every opcode, organized by category.
- [Pseudo-instructions](06-Pseudo-Instructions.md) — how a 32-bit address gets built from two 16-bit halves.

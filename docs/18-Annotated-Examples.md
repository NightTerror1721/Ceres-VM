# Annotated examples

[← Back to index](README.md)

## Walkthrough: `examples/main.casm`

The full source, from [`Ceres/examples/main.casm`](../Ceres/examples/main.casm):

```casm
const TERM_OUT = 0xFF000004
const TERM_BLOCK_ADDR = 0xFF0000F0
const TERM_BLOCK_LEN = 0xFF0000F4
const TERM_BLOCK_CMD = 0xFF0000F8
const BLOCK_CMD_WRITE = 2
const EXIT_CODE = 0x01
const SYS_CTRL = 0xFFFF0000

@rodata
    let HELLO_MSG: u8[16] = "Hello, CeresVM!"

@text
global main:
    la r1, HELLO_MSG
    call print
    li r0, EXIT_CODE
    la r13, SYS_CTRL
    strb [r13 + 0], r0
    ret

println:
.print_loop:
    ldrb r2, [r1]
    cmp r2, 0
    jz .print_end
    la r13, TERM_OUT
    strb [r13 + 0], r2
    add r1, r1, 1
    jp .print_loop
.print_end:
    ret

print:
    mov r3, r1  // Save the original pointer
    call strlen
    mov r2, r0  // Length of the string
    la r13, TERM_BLOCK_ADDR
    str [r13 + 0], r3
    la r13, TERM_BLOCK_LEN
    str [r13 + 0], r2
    la r13, TERM_BLOCK_CMD
    li r0, BLOCK_CMD_WRITE
    str [r13 + 0], r0
    ret

strlen:
    xor r0, r0, r0  // length = 0
.strlen_loop:
    ldrb r2, [r1]
    cmp r2, 0
    jz .strlen_end
    add r0, r0, 1
    add r1, r1, 1
    jp .strlen_loop
.strlen_end:
    ret
```

Build and run it:

```bash
ceres asm examples/main.casm -o hello.cres
ceres run hello.cres
Hello, CeresVM!
```

### Constants block

```casm
const TERM_OUT = 0xFF000004
const TERM_BLOCK_ADDR = 0xFF0000F0
const TERM_BLOCK_LEN = 0xFF0000F4
const TERM_BLOCK_CMD = 0xFF0000F8
const BLOCK_CMD_WRITE = 2
const EXIT_CODE = 0x01
const SYS_CTRL = 0xFFFF0000
```

Named constants for values that would otherwise be unexplained magic numbers scattered through the
file. All six are physical addresses on the MMIO bus: `TERM_OUT` and the three `TERM_BLOCK_*`
registers are offsets within the terminal's slot at `0xFF000000`, already added in since this
example only ever touches one offset per register; `SYS_CTRL` is the system-control device's own
slot at `0xFFFF0000` — see [I/O devices and ports](07-IO-Devices-and-Ports.md). None of these
occupy memory; every use is substituted with the literal value at assembly time (see
[Constants and expressions](13-Constants-and-Expressions.md)).

### Read-only data

```casm
@rodata
    let HELLO_MSG: u8[16] = "Hello, CeresVM!"
```

`"Hello, CeresVM!"` is 15 visible characters; the declaration needs `u8[16]` to leave room for the
implicit trailing `\0` every string literal carries (see
[Data types and literals](11-Data-Types-and-Literals.md)). `@rodata` requires this initializer —
uninitialized read-only data would be pointless.

### `main` — the entry point

```casm
@text
global main:
    la r1, HELLO_MSG
    call print
    li r0, EXIT_CODE
    la r13, SYS_CTRL
    strb [r13 + 0], r0
    ret
```

- `global main:` — `main` must be exactly this name and must be `global`; the linker looks it up by
  that literal string to set `ProgramHeader::entryPoint` (see
  [Labels and symbols](12-Labels-and-Symbols.md)).
- `la r1, HELLO_MSG` — loads the **address** of the string into `r1` (two real instructions, `lui`+
  `ori`, under the hood — see [Pseudo-instructions](06-Pseudo-Instructions.md)).
- `call print` — pushes the return address and jumps to `print`, following the
  register calling convention this file establishes purely by hand: the string pointer is passed in
  `r1` (there's no enforced calling convention in the VM itself — see
  [Known limitations](19-Known-Limitations.md)).
- `li r0, EXIT_CODE; la r13, SYS_CTRL; strb [r13 + 0], r0` — loads the system-control device's MMIO
  base into `r13` and writes `0x01` to its command register (offset `0`), which shuts the VM down
  cleanly (see [I/O devices and ports](07-IO-Devices-and-Ports.md)).
- `ret` here is actually unreachable in practice — the preceding `strb` already stops the machine —
  but it's there so `main` still returns properly if it were ever called as an ordinary subroutine
  instead of being the entry point.

### `println` / `print` — two related routines, on purpose

```casm
println:
.print_loop:
    ldrb r2, [r1]
    cmp r2, 0
    jz .print_end
    la r13, TERM_OUT
    strb [r13 + 0], r2
    add r1, r1, 1
    jp .print_loop
.print_end:
    ret

print:
    mov r3, r1
    call strlen
    mov r2, r0
    la r13, TERM_BLOCK_ADDR
    str [r13 + 0], r3
    la r13, TERM_BLOCK_LEN
    str [r13 + 0], r2
    la r13, TERM_BLOCK_CMD
    li r0, BLOCK_CMD_WRITE
    str [r13 + 0], r0
    ret
```

These demonstrate the two different ways to write the null-terminated string pointed to by `r1`:

- **`println`** loops byte by byte: load a byte (`ldrb`), compare it to zero, jump to `.print_end`
  when the terminator is found, otherwise write the byte to the terminal's `OutputRegister`
  (`strb`) and advance the pointer. This is the "obvious" approach and works on any device, but
  costs several VM instructions per character.
- **`print`** instead computes the string's length once (`call strlen`) and then hands the whole
  block to the device at once: write the source address to `TERM_BLOCK_ADDR`, the length to
  `TERM_BLOCK_LEN`, and finally `2` (write) to `TERM_BLOCK_CMD` — the block-transfer trio every
  MMIO device exposes at its top three offsets (see
  [I/O devices and ports → block-transfer registers](07-IO-Devices-and-Ports.md)). `main` actually
  calls `print`, not `println` — `println` is left in the file as an illustration of the manual
  approach, unused by the rest of the program.

Both routines rely on `.print_loop`/`.strlen_loop` being local to their own enclosing label (`println`
and `strlen` respectively) — see [Labels and symbols](12-Labels-and-Symbols.md) for why two different
subroutines can each have their own `.loop`-style local label with no naming conflict.

### `strlen` — a from-scratch length routine

```casm
strlen:
    xor r0, r0, r0  // length = 0
.strlen_loop:
    ldrb r2, [r1]
    cmp r2, 0
    jz .strlen_end
    add r0, r0, 1
    add r1, r1, 1
    jp .strlen_loop
.strlen_end:
    ret
```

`xor r0, r0, r0` is the idiomatic way to zero a register (no dedicated "zero register" or `mov r0, 0`
convention exists, though `li r0, 0` would work identically). The loop reads one byte at a time,
counts up in `r0` until it finds the terminating `\0`, and returns with the string's length in `r0` —
the by-hand calling convention this file uses: **return value in `r0`**, **string pointer argument in
`r1`**, matching the convention `print` relies on when it does `mov r2, r0` right after `call strlen`.

## Related pages

- [Instruction set](05-Instruction-Set.md) — every mnemonic used above, in full detail.
- [Pseudo-instructions](06-Pseudo-Instructions.md) — what `la` actually expands to.
- [I/O devices and ports](07-IO-Devices-and-Ports.md) — the terminal and system-control devices this example drives.
- [Labels and symbols](12-Labels-and-Symbols.md) — global vs. file-level vs. local labels, as used throughout this file.

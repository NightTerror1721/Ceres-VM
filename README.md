# Ceres

A 32-bit virtual machine with its own instruction set, and an assembler for it. Both are written
in C++23.

```
ceres asm examples/main.casm -o hello.cres
ceres run hello.cres
Hello, CeresVM!
```

## Building

The project builds with MSVC from `Ceres-ASM/Ceres-ASM.vcxproj`, and with GCC or Clang from the
command line. Only the **x64** configurations set `stdcpp23` and the include directory, so build
those.

```sh
cd Ceres-ASM/src
g++ -std=c++23 -I. -o ceres main.cpp vm/*.cpp assembler/*.cpp debug/*.cpp
```

The test suite is a second executable, `Ceres-ASM/Ceres-ASM-Tests.vcxproj`, or:

```sh
sh tests/build.sh
```

`std::print` needs `-lstdc++exp` on MinGW.

## Command line

| Command | What it does |
| --- | --- |
| `ceres asm <source.casm> [-o <out.cres>] [--listing] [--debug]` | Assemble. Without `-o` the source is only checked. `--debug` records the line and symbol tables and, with `-o`, appends them to the `.cres`. |
| `ceres run <file.casm\|file.cres>` | Run, assembling first if given source. `--memory <bytes>` sets the machine size. |
| `ceres disasm <file.casm\|file.cres> [--debug]` | Print the text section as address, encoded word and instruction. With `--debug`, annotated with the source line each word came from. |
| `ceres profile <file.casm\|file.cres>` | Run, then report executed instructions per source line. Time is counted in instructions, so a profile is the same on every run. |
| `ceres debug <file.casm\|file.cres>` | Run under an interactive debugger: breakpoints, stepping by source line, registers, memory, call stack. |

A bare path is shorthand for `run`.

---

# The machine

## Memory

A flat byte array, 16 MiB by default. There is no MMU and no paging.

| Range | Size | Contents |
| --- | --- | --- |
| `0x00000000`–`0x000000FF` | 256 B | Interrupt vector table: 64 entries of 4 bytes. Entry 0 is the reset vector and holds the program's entry point. |
| `0x00000100`–`0x000003FF` | 768 B | BIOS. Currently a three-instruction stub that prints `E` and halts; every fault vector points at it. |
| `0x00000400`– | rest | `.text`, `.rodata`, `.data` and `.bss` in that order, then heap and stack. |

Accesses below `0x400` are rejected for program code, so a program cannot overwrite the vector
table or the BIOS. **`.text` is read-only** as well: a store or a block read that lands anywhere
in the loaded code raises `MemoryFault` and the instruction does not happen, so a pointer that
has gone astray is reported where it goes wrong instead of somewhere else later. Integers are
assembled and disassembled byte by byte in little-endian, so the machine behaves the same
whatever the host's byte order is.

Sections are laid out on 4-byte boundaries and variables are padded to their type's natural
alignment, so a misaligned 16- or 32-bit access raises `AlignmentFault`. Byte accesses never do.

The stack starts at the top of memory and grows down. Pushing below the **stack limit** raises
`StackOverflow`. The loader sets that limit to the end of the loaded image — past `.text`,
`.rodata`, `.data` and `.bss` — so a runaway stack faults instead of eating the program it is
running. With no program loaded the limit is `0x400`, which is all there is to guard.

If the fault has nowhere to push its own two words either, the machine sets the Trap and Halting
flags and stops, rather than re-entering the dispatch forever.

## Registers

Two banks of sixteen 32-bit registers.

| Register | Role |
| --- | --- |
| `r0`–`r12` | General purpose. |
| `r13` / `at` | General purpose, but **the assembler uses it as scratch** when materialising a 32-bit address for `stv` and for `ldv` into a float register. Do not expect it to survive those. `lr` is still accepted as a name for it, but nothing in the machine links through it: `call` pushes the return address on the stack and `ret` pops it. |
| `r14` / `fp` | Named Frame Pointer. Defined, but no instruction touches it. |
| `r15` / `sp` | Stack pointer. Initialised to the top of memory on reset. |
| `f0`–`f15` | 32-bit IEEE-754 floats. They share the instruction's register fields with the integer bank. |

### Flags

| Flag | Set by | Read by |
| --- | --- | --- |
| Zero | Arithmetic, logic, `cmp` | `jz`, `jnz` |
| Sign | Arithmetic, logic, `cmp` | `js`, `jns` |
| Carry | Arithmetic, shifts, `cmp` | `jc`, `jnc`, `adc`, `sbc` |
| Overflow | Signed arithmetic | `jo`, `jno` |
| Interrupt | `sti` / `cli` | Interrupt dispatch: user interrupts (16–63) are dropped while it is clear |
| Halting | `halt` | The step loop |
| Trap | Division by zero, unrecoverable stack fault | Nothing yet |

The halting flag is deliberately **not** saved when an interrupt is taken: `halt` means "wait
for an interrupt", so restoring the bit on `iret` would put the machine straight back to sleep
and nothing could ever wake it.

## Instruction format

Every instruction is exactly 32 bits.

```
 31      24 23    20 19    16 15    12 11                    0
+----------+--------+--------+--------+-----------------------+
|  opcode  | rd/fd  | rs/fs  | rt/ft  |                       |
+----------+--------+--------+--------+-----------------------+
                             |<------------ imm16 ----------->|
           |<------------------- imm24 ---------------------->|
                                              |<--- imm8 ---->|
```

The immediate fields **overlap the register fields**. `imm16` occupies bits 15:0, which is where
`rt` lives, so an instruction cannot use both `rt` and a 16-bit immediate. This is why stores put
the base address in `rd` and the value in `rs`:

```
str [rd + imm16], rs
```

There is no way to load a 32-bit immediate in one instruction. A full address takes `lui` for the
high half followed by `ori` for the low half, which is what `la`, `ldv` and `stv` expand to.

## Instruction set

**If an instruction has a destination, the destination is written first.** A load, a store, a
move, an arithmetic result, a port read — all of them. There is no exception, which is what lets
one mnemonic tell a load from a store: `mov r1, [r2]` reads and `mov [r2], r1` writes, and the
difference is which side the memory is on.

Relative branches take a signed 24-bit displacement, measured **from the branch itself**, giving a
range of ±8 MiB.

### System · `0x00`–`0x07`

`nop` `halt` `trap` `reset` `int imm8` `iret` `cli` `sti`

A program terminates by writing `0x01` to the system control port, not with an instruction. `halt`
suspends the machine until an interrupt arrives; arm the timer and enable interrupts with `sti`
first, or nothing will wake it.

### Arithmetic · `0x10`–`0x2F`, `0xCE`–`0xD1`

`add` `adc` `sub` `sbc` `mul` `imul` `div` `idiv` `mod` `imod` `neg` `mulh` `imulh` `abs` `min`
`imin` `max` `imax` `sqrt`

`mul` keeps the low 32 bits and drops the rest, so an overflow was simply lost; `mulh` and `imulh`
are the half that went missing. `abs` of the smallest `i32` cannot fit, so it answers with that
same value and sets Overflow. `abs` on a pair of float registers picks `FABS`, and `sqrt` only has
a float form.

Each takes three registers or two registers and a 16-bit immediate; the assembler picks the
encoding. The `i`-prefixed forms are signed. Floating-point variants are selected by using float
registers: `add f1, f2, f3` assembles to `FADD`.

Division by zero sets the Trap flag and continues, leaving the destination unchanged.

### Logic and shifts · `0x30`–`0x3F`, `0xC8`–`0xCD`

`and` `or` `xor` `not` `shl` `shr` `sar` `rol` `ror` `clz` `popcnt` `bswap` `sxtb` `sxth`

Shift and rotate amounts use the low five bits of the operand. `clz` of zero is 32. `sxtb` and
`sxth` widen a register the way `ldrsb` and `ldrsh` widen memory, which nothing could do before
without a pair of shifts.

### Memory · `0x40`–`0x4E`, `0xB4`–`0xBD`

`mov` `li` `lui` `ldr` `ldrb` `ldrh` `ldrsb` `ldrsh` `str` `strb` `strh` `la`

```casm
ldrb r2, [r1 + 4]     // load
strb [r5 + 1], r6     // store
```

The access may carry its **width** instead of the mnemonic. `u8[r2 + 4]` is a byte access,
`i16[...]` a sign-extending halfword one, and so on — so one `ldr` and one `str` cover what the
whole `ldrb`/`ldrh`/`ldrsb`/`ldrsh` family covers:

```casm
ldr r1, u8[r2 + 4]    // LDRB
ldr r1, i16[r2 + r3]  // LDRSHX - an index works too
str r1, u8[r2 + 4]    // STRB
```

This is the same thing `ldv` and `stv` already do with a variable's declared type; a bare
`[r2 + 4]` had no type to read it from. Any scalar works, aliases included — `byte[r2]` is
`u8[r2]`. The width-suffixed mnemonics keep working unchanged.

A **symbol in brackets** is its contents, the way brackets mean contents everywhere else; the bare
name is its address:

```casm
mov r1, counter       // the address of counter
mov r1, [counter]     // what counter holds
mov [counter], r1     // write it back
```

This is the convention of NASM, the 68000 and the Z80. It also means `ldv`/`stv` are now spellings
of `ldr`/`str` with a bracketed name — and they relax to one PC-relative word just the same.

### `mov` moves anything anywhere

Every form below is an existing instruction chosen by the shape of its operands, so `mov` is one
name for the whole of moving:

```casm
mov r1, r2            mov r1, 42            mov f1, f2
mov r1, counter       mov r1, [counter]     mov [counter], r1
mov r1, [r2 + 4]      mov [r2 + 4], r1      mov r1, u8[r2 + r3]
mov u16[r2 + 4], r1   mov f1, f32[r2 + 4]   mov f32[r2 + 4], f1
```

What `mov` deliberately will not do is choose between `mtf` and `itof` — copying a bit pattern and
converting a number have the same shape and opposite meanings, so they keep their own names.

The offset may be a **register** instead, which is an index rather than a displacement and picks
an opcode of its own — the same mnemonic either way, exactly as `add` chooses between a register
and an immediate:

```casm
ldrb r2, [r1 + r3]    // LDRBX: no `add r4, r1, r3` before every element
strb [r5 + r3], r6    // STRBX
```

An index cannot be subtracted — there is no opcode that subtracts one — and it cannot be a float
register. A displacement and an index cannot appear together: `[r1 + r3 + 4]` does not parse.

### Control flow · `0x50`–`0x79`

`jp` `jz` `jnz` `jc` `jnc` `js` `jns` `jo` `jno` `call` `ret` `cmp`

Every conditional branch has a register form, chosen automatically when the operand is a register
rather than a label.

The single-flag branches above cannot express an ordering: `cmp` leaves Zero, Sign, Carry and
Overflow set, but "greater" needs Sign against Overflow and "above" needs Carry against Zero. Eight
more branches read two flags each:

| Signed | Unsigned | Condition |
| --- | --- | --- |
| `jgr` | `jab` | greater than |
| `jge` | `jae` | greater than or equal |
| `jls` | `jbl` | less than |
| `jle` | `jbe` | less than or equal |

`jeq` and `jne` are aliases of `jz` and `jnz`, and `jmp` is an alias of `jp`.

#### `bl` — calling without the stack

`call` pushes a return address and `ret` pops it; that is unchanged and always available. `bl`
puts the return address in a **register you name** instead, and the return is the `jp` that
already exists:

```casm
    li r0, 79
    bl r11, print_char      // r11 = the instruction after this one
    ...
print_char:
    outb 0x01, r0
    jp r11
```

A leaf costs no memory traffic at all, and a tail call is a jump. The second operand may be a
register (`bl r11, r5`), which is how you call through a pointer.

The link register is an **operand, not a fixed one**, so `bl` reserves nothing and competes with
nothing — which is why `r13` could become the assembler's temporary. The cost is reach: `rd` takes
four bits off the displacement, so a labelled `bl` reaches ±512 KiB where `call` reaches ±8 MiB.
Further than that is an error naming `call` as the way out.

Nothing saves the register for you. A function that calls anything else has to keep its own link
somewhere, which is exactly the discipline `call`/`ret` does for you — so `bl` is for leaves.

### Stack · `0x80`–`0x89`

`push` `pop` `pushm` `popm` `enter` `leave`

`push` with **no operand** is the flags register — nothing else `push` takes has no operand, so
the signature says which is meant. `pushf` and `popf` still parse and mean exactly that.

`enter N` saves `fp`, points it at the saved word and opens a frame of `N` bytes; `leave` undoes
all three. A bare `enter` opens a frame of nothing, for a function whose locals live in registers.
`enter` is all or nothing: a prologue that saved `fp` and then found no room for the frame would
leave the function running on a stack it does not own.

`pushm` and `popm` take a **bit per register**, which is what `imm16` has exactly sixteen of:

```casm
    pushm 0x0F00        // r8, r9, r10 and r11, in one instruction
    ...
    popm  0x0F00        // and back, from the same four slots
```

`pushm` stores from the highest set bit down, so the lowest-numbered register lands at the lowest
address and `popm` walks back up in order — a matching pair restores what it saved whatever the
mask. It is all or nothing: if the whole mask does not fit, nothing is pushed and `StackOverflow`
is raised, because a half-saved frame would be restored as if it were whole.

### Conversions · `0x90`–`0x95`

`itof` `iitof` `ftoi` `ftoii` `mtf` `mff`

`mtf` and `mff` move the bit pattern without converting.

### I/O · `0xA0`–`0xB3`

`in` `inb` `inh` `insb` `insh` `inm` `out` `outb` `outh` `outm`

The port is either an 8-bit immediate or a register; the assembler picks the encoding from the
operand. Block forms take the port, an address and a size:

```casm
outm 0x01, r3, r2     // destination first: the port, then the address and the size
inm  r3, 0x02, r2     // destination first: the memory, then the port and the size
```

## Ports

| Port | Device |
| --- | --- |
| `0x00`–`0x03` | Terminal: status, output, input, debug hex. **Implemented.** |
| `0x10`–`0x12` | Timer: tick count, real-time clock, command. **Implemented.** |
| `0x20`–`0x23` | Disk. |
| `0x30`–`0x33` | GPU. |
| `0x40`–`0x43` | Mouse and gamepad. |
| `0x50`–`0x51` | Audio. |
| `0x60`–`0x62` | Network. |
| `0xFE` | Random number source. |
| `0xFF` | System control. **Implemented:** write `0x01` to shut down, `0x02` to reset. |

Reading a port with no device attached returns all ones.

---

# The language

## Sections

```casm
@text     // code
@rodata   // immutable data
@data     // initialised mutable data
@bss      // zero-filled at load time, occupies no space in the file
```

## Constants and variables

```casm
const MAX_PLAYERS = 4
const BLOCK       = 16 * 4          // + - * / with the usual precedence

@rodata
    let greeting: u8[16] = "Hello, CeresVM!"
    let pi:       f32    = 3.14159

@data
    let scores:   i16[4] = [42, -10, 0x1A, 0b10]

@bss
    let buffer:   u32[128]
    let active:   u8[MAX_PLAYERS]
```

Scalar types are `u8`, `u16`, `u32`, `i8`, `i16`, `i32` and `f32`, plus aliases:

| Alias | Is | For |
| --- | --- | --- |
| `char` | `u8` | A character |
| `bool` | `u8` | `true` or `false`, and nothing else |
| `string` | `u8[]` | A run of characters with a terminating zero |
| `ptr` | `u32` | A memory address |
| `port` | `u8` | An I/O port number |
| `irq` | `u8` | An interrupt vector number, `0`–`63` |
| `byte` `half` `word` | `u8` `u16` `u32` | The machine's own vocabulary, so a declaration reads like the `ldrb`/`ldrh`/`ldr` that uses it |

An alias is the same type as what it stands for — `ptr` loads with `ldr` like any `u32` — but the
spelling is kept, so diagnostics say `irq` rather than `u8`. Three of them promise a range that the
underlying scalar does not, and that promise is enforced:

```casm
    let vector: irq  = 99      // error: the largest is 63
    let flag:   bool = 5       // error: the largest is 1
```

Every alias is a word that identifiers give up, which is why the list stops here. An integer literal has no type of its own; it takes the declared one, and is
rejected if it does not fit. A string literal is stored with a terminating zero, so
`"Hello, CeresVM!"` needs `u8[16]`.

A declaration may be longer than its initialiser; the remainder is zero. Variables are padded to
their type's natural alignment.

### Multidimensional arrays

```casm
@data
    let grid:  i32[2][3] = [[1, 2, 3], [4, 5, 6]]
    let a:     i32[][]   = [[1, 2, 3], [4, 5, 6]]   // both sizes worked out
    let b:     i32[2][]  = [[1, 2, 3], [4, 5, 6]]   // only the inner one
    let names: u8[][8]   = ["ada", "grace"]         // a string fills a row
@bss
    let cube:  i16[4][4][4]
```

**Any** dimension may be left empty, not just the outermost. A size is only an error when the
initialiser cannot supply it:

- A **declared** dimension wins, and a short row is padded with zeroes.
- An **omitted** dimension is read off the initialiser, so every row at that level has to be the
  same length — an irregular one is precisely what makes the size unknowable.
- With **no initialiser** there is nothing to read, so every dimension needs a size.
- A flat initialiser is accepted for a multidimensional type when every size is written down.

Dimensions run outermost first, elements are stored row-major, and the rank is capped at four.

### Directives

```casm
@data
    let header: u8 = 1
    align 16            // pad up to a 16-byte boundary
    let body:   u8 = 2
    org 64              // pad up to offset 64 within this section

assert Frame % 4 == 0
assert BLOCK >= 8, "a block smaller than eight will not fit"
```

`align` takes a power of two. `org` is an offset **within the section**, not an absolute address
— the linker places sections — and it only moves forward; going back would write over what is
already there. Both pad with zeroes.

`assert` fails the build when its expression is zero, with the message if one was given. It is
what makes part of the calling convention checkable rather than merely written down.

### Constant expressions

```casm
const BLOCK   = 64
const HEADER  = 8
const PAYLOAD = BLOCK - HEADER      // constants may refer to earlier constants

@bss
    let buffer: u8[BLOCK * 2]       // and to size an array

@text
    li r1, sizeof(buffer)           // bytes
    li r2, countof(buffer)          // elements
    li r3, dimof(grid, 1)           // the length of one dimension
```

`+ - * / %` with the usual precedence, then a single comparison — `==` `!=` `<` `<=` `>` `>=`,
answering 1 or 0 — and parentheses. Comparisons do not chain: `a < b < c` would compare a truth
value against `c`, which never means what it looks like. A constant is evaluated when its own
declaration is reached, so it can only refer to constants already declared above it — which is also
why a cycle cannot form.

`sizeof`, `countof` and `dimof` are the only way to recover a dimension that was never written
down, which is what an inferred array size is.

A `const` occupies no memory; it is substituted at its point of use.

### Visibility

A constant, a variable or a macro is private to the file that declares it unless it carries the
`global` prefix, exactly like a label:

```casm
global const MAX_PLAYERS = 4        // visible to any file importing this one
const INTERNAL_SLACK    = 8         // private to this file

@data
    global let scoreboard: u32[8]   // exported, address and all
    let scratch:           u32[8]   // private

global macro print_char $reg, $code // exported
    li $reg, $code
    outb 0x01, $reg
endmacro
```

`global` is what exports; nothing else does. Referring to a name that a module declares without it
says so rather than reporting an unresolved symbol:

```
'MAX_PLAYERS' is declared in 'lib/rules.casm' but is not global, so it is not visible here.
```

## Labels

```casm
global main:      // exported to the linker; `main` is the required entry point
helper:           // visible within the file
.loop:            // local to the enclosing non-local label
```

A local label is stored as `parent.name`, so each subroutine can have its own `.loop`.

### Symbols the linker defines

Nine labels come from the layout rather than from any file, so a program can ask where it ends:

```casm
    la r1, __heap_start       // the first free byte above the program
    la r2, __text_end
```

`__text_start` `__text_end` `__rodata_start` `__rodata_end` `__data_start` `__data_end`
`__bss_start` `__bss_end`, and `__heap_start`, which is `__bss_end` under the name that says what
it is for. Declaring one of these yourself is an error rather than a redefinition.

There is no `__stack_top`: the stack starts at the top of memory, and how much memory there is is
chosen with `--memory` long after the link. Read `sp` on entry instead, before anything has
pushed.

## Addressing

```casm
ldr r1, [r2]
ldr r1, [r2 + 4]
ldr r1, [r2 - 8]          // signed: eight bytes *below* the base
ldr r1, [r2 + OFFSET]     // OFFSET must be a constant
```

A displacement is a signed 16-bit field, `-32768` to `32767`. Anything outside that is rejected
rather than truncated.

Write the spaces: `[r5+0]` lexes the `+0` as a signed literal and fails to parse.

## Pseudo-instructions

| Written | Expands to | Size |
| --- | --- | --- |
| `la rd, symbol` | `lui` + `ori` | 8 B |
| `la rd, "text"` | an anonymous `.rodata` string, then its address | 8 B |
| `ldv fd, 1.5` | an anonymous `.rodata` float, then a load of it | 4 B or 12 B |
| `ldv rd, variable` | one PC-relative load, or `lui` + `ori` + a load | 4 B or 12 B |
| `stv rs, variable` | one PC-relative store, or `lui` + `ori` + a store | 4 B or 12 B |
| `ldvp` / `stvp` | the PC-relative form, demanded rather than hoped for | 4 B |
| `neg rd, rs` | `imul rd, rs, -1`, or `fneg` for float registers | 4 B |
| `la rd, imm32` | `lui` + `ori` | 8 B |
| `la rd, [rs + imm16]` | `lea` | 4 B |
| `ifXX rs, rt, label` | `cmp` (or `cmpi`, or `fcmp`) + the matching branch | 8 B |
| `swap rd, rs` | three `xor`s, using no temporary | 12 B |
| `inc rd` / `dec rd` | `addi rd, rd, 1` / `subi rd, rd, 1` | 4 B |
| `clr rd` | `li rd, 0` | 4 B |
| `tst rs` | `cmpi rs, 0` | 4 B |
| `jmp label` | `jp` | 4 B |

`la` is **putting an address in a register**, however the address is written: the address of a
symbol, a full 32-bit literal (`lui` + `ori` for both), or `[rs + imm16]` computed at run time
(one word, the real `LEA` opcode). The operand's type is what tells them apart. `lc` and `lea`
still parse, as the spellings these had when they were separate.

The two lengths in one mnemonic are why a statement reserves the size **its own signature** needs
rather than the largest its mnemonic can be: otherwise every `la rd, [rs + imm16]` would reserve
eight bytes and pad with a `nop`.

`ifXX` writes the comparison and the branch as one instruction, and covers every condition the
branches do — `ifeq` `ifne` `ifgr` `ifge` `ifls` `ifle` `ifab` `ifae` `ifbl` `ifbe`. The second
operand may be a register or an immediate, and a pair of float registers picks `fcmp`:

```casm
    ifls r1, r2, .smaller       // signed
    ifge r3, 100, .at_least     // against an immediate
    ifgr f0, f1, .bigger        // fcmp, without remembering which flag it sets
```

**`ldv` and `stv` have two sizes.** When the variable is within ±32 KiB of the instruction — which
in a program of ordinary size is all of them — the linker rewrites them into a single PC-relative
word, measured from the instruction itself the way a branch is. Otherwise they stay as `lui` +
`ori` + the access, and only then do they clobber `at` (`r13`).

The linker can choose because it lays the program out twice: once to find out where everything is,
and again after shortening. One pass is enough because all of `.text` comes before all of the data,
so every reference points forward and shortening can only ever bring things closer.

`ldvp` and `stvp` are the short form asked for by name: same encoding, but a variable out of reach
is an error instead of a longer instruction. Write them when four bytes is a requirement rather
than a preference.

## Macros

```casm
macro print_char $reg, $code
    li $reg, $code
    outb 0x01, $reg
endmacro

macro count_down $reg, $from
    li $reg, $from
%%loop:                        // unique to each expansion
    sub $reg, $reg, 1
    cmp $reg, 0
    jnz %%loop
endmacro

@text
global main:
    print_char r1, 72
    count_down r2, 3
```

Macros are keyed by name **and** argument count, so one name can carry several arities. A
`%%label` is rewritten with the expansion's number appended, so using a macro twice in one scope
does not redefine its internal labels. Macros may call other macros; runaway recursion is reported
rather than hanging.

## The timer

```casm
    li r1, 1000
    out 0x12, r1        // fire an interrupt in 1000 instructions
    sti                 // user interrupts are masked until this
    halt                // suspended until the timer fires
```

Time is counted in **executed instructions**, not wall clock, so a program behaves the same on
every run. Writing 0 disarms the timer; setting the high bit asks for a periodic one that re-arms
itself. The timer requests `UserInterrupt0` (16), whose vector lives at address `0x40`.

`0x10` reads the tick count and `0x11` the real time in seconds, which is the one thing here that
is not deterministic.

## Register aliases

```casm
alias cursor = r5
alias total  = r6

@text
global main:
    clr  total
    ldrb total, [cursor + 1]
```

A name for a register, usable anywhere one can be written, including as a memory base. Resolved by
the parser, so it is file-scoped and nothing downstream ever sees it.

## Structs

```casm
struct Entity
    x:      i32
    y:      i32
    health: u16
    flags:  u8
endstruct

@bss
    let player: u8[Entity]
    let mobs:   u8[32][Entity]

@text
    ldr r2, [r1 + Entity.y]
```

A struct's name is also a type: `let player: Entity` is `let player: u8[Entity]`, and
`Entity[4]` is `u8[4][Entity]`. A field may be a struct too, and its initialiser nests to match.

A `struct` reserves no storage. It declares one constant per field, holding that field's byte
offset, plus the struct's own name holding the total size — rounded up to the widest field, so an
array of them stays aligned. `Entity.y` is therefore an ordinary constant displacement and
`u8[32][Entity]` an ordinary array. `global struct` exports the lot.

A `u8[Entity]` in `@data`/`@rodata` accepts a positional initializer — values map to fields in
order, each checked against its field's type, padding zero-filled:
`let player: u8[Entity] = [10, 20, 100, 1]`.

## Unused private declarations

A `const`, `let` or `macro` that is not `global` and that nothing in its own file names cannot be
reached from anywhere, so it is reported — as a warning, which does not stop the build:

```
warning [game.casm:4] 'SLACK' is declared but never used, and is not global, so nothing
                      outside this file can use it either
```

## Imports

```casm
import "lib/math.casm"
import "lib/math.casm" as math      // and now math.PI, math.clamp
```

A relative path resolves against the importing file. What becomes visible is whatever the module
declares `global`: constants, variables and macros alike. Anything else stays inside it.

A module is read, parsed and built exactly once per run, and an `import` is a reference to it rather
than a copy of its tables — so importing the same module twice, directly or by two different routes,
is a no-op rather than a redefinition. Import cycles are still reported.

When two modules export the same name and a file imports both, the clash is reported at the use,
naming both files. Naming the imports is the way out: a qualified name is answered by exactly one
module, so `math.LIMIT` and `fx.LIMIT` can both be reached from the same file. `as` is matched as an
ordinary identifier, so it stays usable as a name everywhere else.

---

# Status

The assembler and the VM work end to end. What is not done:

- **Nothing enforces the calling convention.** It is written down — register roles, a frame shape
  and the two macros that open and close it, in `lib/call.casm` — but no macro can verify that you
  preserved `r8` or left `sp` where you found it.
- **Devices.** Twenty-six ports are reserved and seven are implemented. Disk, GPU, input, audio
  and network are all still stubs.
- **`parseOperand` gaps.** Float, character and string literals are not accepted as operands, and
  a `%%label` cannot start a statement outside a macro body.
- **Reverse debugging has a horizon.** The debugger ([wiki](docs/22-Debugger.md)) runs backwards,
  but only as far as the recording reaches — 1.28 million instructions by default — and recording
  costs one copy of the machine's memory.

## Tests

173 cases, 777 assertions, run with `sh tests/build.sh`. CI builds with MSVC and GCC 15 and runs
the suite on both.

A test that pins a bug which is still open is marked `TEST_KNOWN_FAILURE`: it asserts the correct
behaviour and is expected to fail, so the suite stays green and turns **red** when the bug is
fixed, which is the signal to delete the marker.

# Practical tutorial: from "Hello, Ceres" to two terminal games

[← Back to index](README.md)

> This tutorial walks through CeresASM with progressive exercises that build up to two playable
> terminal games (rock-paper-scissors and tic-tac-toe). The rest of the wiki (pages 01-19) is the
> reference; this page is the guided path that walks it in practice.

All the code for this tutorial lives in
[`Ceres/examples/tutorial/`](../Ceres/examples/tutorial/), ready to assemble and
run as-is. Each exercise is a standalone `.casm` file with comments and a
"TRY IT YOURSELF" section at the top to extend it.

## 0. Before anything else: build `ceres` and get keyboard input that actually works

Follow the [`README`](../README.md) to build:

```sh
cd Ceres
cmake --preset gcc && cmake --build --preset gcc-debug
```

The binary lands at `Ceres/build/gcc/bin/Debug/ceres`.

**Important note if your copy of `ceres` predates this tutorial:** `ceres run` didn't read the
real keyboard. The terminal device (`TerminalDevice`, port `0x02`) always had an input buffer
meant to receive data concurrently (`pushInput`, protected with atomics), but nothing in
`main.cpp` connected it to `stdin`. Without that, no interactive program — and so neither of the
two games — could read what the player types. A background thread has been added in
`runProgram()` that reads from `stdin` and calls `terminal.pushInput()`, reusing the buffer that
already existed. If your binary is newer than this change, you already have working keyboard
input with `ceres run game.casm`; if not, rebuild it.

Check that it works:

```bash
ceres run examples/tutorial/02_eco.casm
```

Type a sentence and press Enter: the program echoes it back character by character.

## 1. How keyboard input works in Ceres (read this before exercise 2)

Ceres's terminal is **line-buffered**, just like any shell: a program receives nothing on port
`0x02` (`TERM_IN`) until the user presses Enter, and then it receives the whole line at once,
including the `\n` itself (code 10). Port `0x00` (`TERM_STATUS`) has bit 0 set to one when at
least one byte is pending; reading `TERM_IN` without checking that first simply returns `0` if
there's nothing there.

This has a practical consequence: if your program only reads **one** character per turn (for
example, a rock-paper-scissors move), the `\n` the user typed afterward stays in the buffer and
sneaks in as if it were the next data read. The solution every exercise from 3 onward uses is a
`read_command` subroutine that reads the first character it gets, and then **discards the rest of
the line until it finds the `\n`**:

```casm
read_command:
.wait_first:
    inb TERM_STATUS, r1
    and r1, r1, 1
    jz .wait_first
    inb TERM_IN, r0        // r0 = first byte (return value)
.flush:
    inb TERM_STATUS, r1
    and r1, r1, 1
    jz .flush
    inb TERM_IN, r2
    cmp r2, 10
    jnz .flush
    ret
```

## 2. Two corrections to the instruction reference

While writing the exercises, two spots turned up where the real code (verified by assembling and
running it) doesn't match what
[05 · Instruction set](05-Instruction-Set.md) says — already fixed on that page, but worth
explaining here because it's easy to trip over the same thing:

- **`in`/`inb`/`inh`/`insb`/`insh`**: the port goes **first**, the destination register
  **second** — `inb PORT, rd`, not `inb rd, PORT` as the table said. It's consistent with `out`
  (`outb PORT, rs`): in both cases the port is written first.
- **`str`/`strb`/`strh`**: the value goes **first**, the bracketed address **second** —
  `strb rs, [rd + imm16]`, not `strb [rd + imm16], rs`. In fact, the
  [`examples/test.casm`](../Ceres/examples/test.casm) example had this exact mistake and didn't
  even assemble; it's been fixed too.

This tutorial's exercises already use the correct order in both cases, verified by assembling and
running each one.

## 3. The exercises

| # | File | What it practices |
| --- | --- | --- |
| 1 | [`01_hola.casm`](../Ceres/examples/tutorial/01_hola.casm) | `@rodata`, `la`, printing a string through the terminal port |
| 2 | [`02_eco.casm`](../Ceres/examples/tutorial/02_eco.casm) | Reading from `TERM_IN`/`TERM_STATUS`, byte by byte |
| 3 | [`03_suma.casm`](../Ceres/examples/tutorial/03_suma.casm) | `read_command`, ASCII↔number conversion, `cmp`+`jc` |
| 4 | [`04_cuenta_atras.casm`](../Ceres/examples/tutorial/04_cuenta_atras.casm) | Loops with `cmp`/`jz`/`jnz` |
| 5 | [`05_notas.casm`](../Ceres/examples/tutorial/05_notas.casm) | Arrays in `@data`, `[reg + offset]` addressing, `div`/`mod` |
| 6 | [`06_macros.casm`](../Ceres/examples/tutorial/06_macros.casm) | `macro`, the `proc_enter`/`proc_leave` calling convention |
| 7 | [`07_aleatorio.casm`](../Ceres/examples/tutorial/07_aleatorio.casm) | A linear congruential generator seeded with `RTC_TIME` |
| 8 | [`08_pantalla.casm`](../Ceres/examples/tutorial/08_pantalla.casm) | The framebuffer: setting the grid, drawing into memory and flushing it with `outm` |
| 9 | [`09_disco.casm`](../Ceres/examples/tutorial/09_disco.casm) | The disk: selecting a sector, `inm`/`outm`, checking `DISK_STATUS` and flushing to a file |

For each one:

```bash
ceres run examples/tutorial/0N_name.casm
```

9 is the exception: without `--disk` the disk exists but forgets everything when the machine
stops, so the counter will always say "1". With a file behind it, it survives:

```bash
ceres run examples/tutorial/09_disco.casm --disk saves.img
```

and read the header comment before looking at the code: it poses a specific challenge for you to
solve before seeing how this tutorial solved it.

### Note on exercise 7: why the seed is `RTC_TIME` (port `0x11`) and not `SYS_TICKS` (port `0x10`)

Ceres has no real source of random numbers — port `0xFE` is reserved but not implemented (see
[07 · I/O devices and ports](07-IO-Devices-and-Ports.md)). The temptation is to seed a generator
of your own with the count of executed instructions (`SYS_TICKS`), but
[02 · Memory](02-Memory.md) and [07 · I/O devices and ports](07-IO-Devices-and-Ports.md) already
point out that time in Ceres is counted in executed instructions, not real time, precisely so a
program behaves the same way on every run — and that includes `SYS_TICKS`. Seeding with it gives
literally the same "randomness" every time you run the program (you can check this by running
`07_aleatorio.casm` twice in a row). The only value in the whole machine that genuinely changes is
the wall clock in seconds, `RTC_TIME` (port `0x11`) — the wiki itself points it out as "the only
thing here that isn't deterministic". That's why it's the seed exercise 7 uses, and later,
rock-paper-scissors too.

## 4. The two games

### Rock, paper, scissors — [`piedra_papel_tijera.casm`](../Ceres/examples/tutorial/piedra_papel_tijera.casm)

Against the machine. Brings together keyboard input (2-3), arithmetic with carry (3-4), a
scoreboard in memory (5), macros (6) and the random generator (7). The rule for who wins comes
down to a single modular calculation: with `0=Rock, 1=Paper, 2=Scissors`, move `i` beats move
`(i - 1 + 3) mod 3`, so

```casm
sub r6, player, machine
add r6, r6, 3
mod r6, r6, 3      // 0 = draw, 1 = win, 2 = lose
```

decides the round without a cascade of comparisons for the 9 possible combinations.

```bash
ceres run examples/tutorial/piedra_papel_tijera.casm
```

### Tic-tac-toe — [`tres_en_raya.casm`](../Ceres/examples/tutorial/tres_en_raya.casm)

Two human players taking turns on the same terminal (no AI — challenge 3 in its header suggests
adding one by reusing `next_random`). The board is a 9-byte array in `@data`
(`0`=empty, `1`=X, `2`=O) addressed with offsets computed at runtime (unlike the grades array from
exercise 5, here the index isn't known until the player types it). Checking who won walks a table
of 8 winning combinations (`WIN_LINES`, in `@rodata`) with a loop, instead of hand-writing the 8
comparisons.

```bash
ceres run examples/tutorial/tres_en_raya.casm
```

**An easy trap to fall into** when writing subroutines like `check_winner` that use several
registers as scratch variables: if the calling code still needs the value a register held
*before* the call (for example, whose turn it was, or the result the previous subroutine just
returned), you have to save it in a register the subroutine doesn't touch — or reread it from
memory — **before** making any other call. `tres_en_raya.casm` ran into exactly this twice while
being written (the turn never switched, and the winner was announced backwards) because
`print_board` and `check_winner` reuse registers the main loop still needed; the `mov r11, r0` and
`ldv r4, turn` comments you'll see in the code are the fix. It's the same problem the
`proc_enter`/`proc_leave` convention from exercise 6 solves, applied to why a convention is needed
in the first place.

## 5. From here on

- [14 · Macros](14-Macros.md) — the full calling convention (`r0`-`r3`, `r4`-`r9`) exercise 6 takes
  its idea from.
- [15 · Modules and import](15-Modules-and-Import.md) — a natural challenge: pull `read_command`,
  `print`, `strlen` and `next_random` out into a shared `runtime.casm` file and import it from
  both games with `import`, instead of duplicating them in each one.
- [19 · Known limitations](19-Known-Limitations.md) — why there's no calling convention enforced
  by the machine, and what other devices (besides the terminal and the timer) still aren't
  implemented.

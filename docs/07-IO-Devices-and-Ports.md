# I/O devices and ports

[← Back to index](README.md)

Ceres has no memory-mapped I/O: devices are reached exclusively through the `in`/`out` family of
instructions (see [Instruction set → I/O operations](05-Instruction-Set.md#io-operations-0x90-0xa3)),
addressing one of 256 8-bit **ports**. `IOPorts` (in
[`io_ports.h`](../Ceres-ASM/src/vm/io_ports.h)) holds a `std::array<IODevice*, 256>` and dispatches
reads/writes to whichever device is attached at a given port — or synthesizes a default response if
nothing is attached there.

## Behaviour of an unattached port

- **Reading** an unattached port returns all-ones: `0xFF` for a byte, `0xFFFF` for a halfword,
  `0xFFFFFFFF` for a word. A signed read returns `-1`, which has the same bit pattern.
  A block read (`inm`/`inrm`) fills the destination memory with `0xFF` bytes.
- **Writing** to an unattached port is silently discarded — no fault, no effect.

## The default port map

This is the layout `default_ports` in [`io_ports.h`](../Ceres-ASM/src/vm/io_ports.h) reserves.
"Implemented" means a device in [`devices.h`](../Ceres-ASM/src/vm/devices.h) actually backs it today;
everything else currently behaves as an unattached port (all-ones on read, no-op on write) until a
device is written and wired up.

| Port(s) | Device | Status |
| --- | --- | --- |
| `0x00` | Terminal status | **Implemented** |
| `0x01` | Terminal output | **Implemented** |
| `0x02` | Terminal input | **Implemented** |
| `0x03` | Debug hex output | Reserved, not implemented |
| `0x10` | Timer: tick count | **Implemented** |
| `0x11` | Timer: real-time clock | **Implemented** |
| `0x12` | Timer: command (arm/disarm) | **Implemented** |
| `0x20`–`0x23` | Disk (status, command, sector, data) | Reserved, not implemented |
| `0x30`–`0x33` | GPU (command, width, height, sprite data) | Reserved, not implemented |
| `0x40`–`0x43` | Mouse and gamepad | Reserved, not implemented |
| `0x50`–`0x51` | Audio (command, frequency) | Reserved, not implemented |
| `0x60`–`0x62` | Network (status, send, receive) | Reserved, not implemented |
| `0x70`–`0xEF` | — | Reserved for future default devices |
| `0xFE` | Random number source | Reserved, not implemented |
| `0xFF` | System control | **Implemented** |

Of the 26 ports the default map reserves, 7 have a working device behind them today; disk, GPU,
input devices, audio and network are all still stubs — see
[Known limitations](19-Known-Limitations.md).

## Devices implemented today

### `SystemControlDevice` (port `0xFF`, write-only)

The only way to stop the VM cleanly. There is no `exit`/`quit` instruction — a program terminates by
writing a command byte to the system control port:

| Value written | Effect |
| --- | --- |
| `0x01` | Shuts the machine down (invokes the shutdown callback the host registered — `ceres run` uses this to stop its run loop). |
| `0x02` | Resets the machine (invokes the reset callback). |
| anything else | Ignored. |

```casm
li r0, 0x01
out 0xFF, r0    // halt the VM cleanly
```

Reading from this port returns all-ones like any other write-only device would if read.

### `TimerDevice` (ports `0x10`–`0x12`)

Gives the machine its only source of asynchronous interrupts — without it, `halt` would suspend the
machine forever, since nothing could ever wake it back up.

| Port | Direction | Meaning |
| --- | --- | --- |
| `0x10` (`SYS_TICKS`) | Read | Number of instructions executed so far (a `u64` counter, truncated to 32 bits on read). |
| `0x11` (`RTC_TIME`) | Read | Wall-clock seconds since the Unix epoch. **This is the one value in the entire VM that is not deterministic** — everything else (including `SYS_TICKS`) behaves identically on every run. |
| `0x12` (`TIMER_CMD`) | Write | Arms or disarms the timer. |

Writing to the command port:

- The low 31 bits are the number of **instructions** (not milliseconds) until the timer fires.
- The high bit (`0x80000000`), if set, makes the timer **periodic**: it automatically re-arms itself
  with the same period every time it expires.
- Writing `0` disarms the timer.

When the timer expires it raises `UserInterrupt0` (interrupt number 16) — see
[Interrupts and exceptions](08-Interrupts-and-Exceptions.md). Since that's a user interrupt (not one
of the 16 reserved/always-deliverable ones), **the Interrupt flag must be set (`sti`) or the
interrupt is dropped** the moment it fires, never queued for later.

Typical wake-up-after-a-delay pattern:

```casm
li r1, 1000
out 0x12, r1    // fire in 1000 executed instructions
sti             // user interrupts must be unmasked, or the timer's interrupt is lost
halt            // suspended until the timer (or any other interrupt) fires
```

Because time here is counted in *executed instructions* rather than wall-clock time, a program's
behaviour is fully reproducible from run to run and independent of host machine speed — with the
sole exception of anything that reads `RTC_TIME`.

### `TerminalDevice` (ports `0x00`–`0x02`)

A minimal character terminal.

| Port | Direction | Meaning |
| --- | --- | --- |
| `0x00` (`TERM_STATUS`) | Read | Bit 0 (`0x01`) set when input is available to read; bit 1 (`0x02`) is always set (the terminal is always ready to accept output in this simple implementation). |
| `0x01` (`TERM_OUT`) | Write | Writes a byte (or more, via the halfword/word/block forms) straight to the process's standard output as characters. |
| `0x02` (`TERM_IN`) | Read | Reads the next byte from a small 64-byte input ring buffer (`pushInput()`, called by the host embedding the VM), or `0` if nothing is buffered. |

```casm
// print one character
li r0, 'H'
outb 0x01, r0

// print the null-terminated string pointed to by r1
.print_loop:
    ldrb r2, [r1]
    cmp r2, 0
    jz .print_end
    outb 0x01, r2
    add r1, r1, 1
    jp .print_loop
.print_end:
```

The block forms (`outm`, `inm`) work here too: `outm 0x01, r_addr, r_size` prints `r_size` bytes
starting at `r_addr` in one instruction, instead of looping byte by byte — this is exactly what
[`examples/main.casm`](../Ceres-ASM/examples/main.casm)'s `print` routine does.

## Related pages

- [Instruction set → I/O operations](05-Instruction-Set.md#io-operations-0x90-0xa3) — the `in`/`out` family.
- [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — how the timer's interrupt reaches your handler.
- [Known limitations](19-Known-Limitations.md) — which devices are still stubs.

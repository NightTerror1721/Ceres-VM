# I/O devices and MMIO

[← Back to index](README.md)

Ceres has no port-based I/O any more: devices are reached exclusively through ordinary loads and
stores (`ldr`/`str` and the rest — see [Instruction set](05-Instruction-Set.md)), at addresses inside
a window reserved for them at the top of the physical address space. `MmioBus` (in
[`mmio_bus.h`](../Ceres/libs/vm/include/ceres/vm/mmio_bus.h)) holds up to 256 devices, each in its own 64 KiB slot,
and dispatches a load or store that lands inside one to whichever device claims it — or synthesizes a
default response if nothing is attached there.

This costs nothing: `Memory::MaxSize` is 1 GiB, and the MMIO window sits at `0xFF000000`, which no
configuration of RAM can ever reach. A device's registers and a program's own memory can never
collide.

## The address map

```
0x00000000                                                              0xFFFFFFFF
├─────────────────────── ordinary memory (up to 1 GiB) ──────────────┤├── MMIO ──┤
                                                                        0xFF000000
```

| Base | Slot | Device |
| --- | --- | --- |
| `0xFF000000` | 0 | Terminal |
| `0xFF010000` | 1 | Timer |
| `0xFF020000` | 2 | Disk |
| `0xFF030000` | 3 | Framebuffer |
| `0xFF040000` | 4 | DMA controller |
| `0xFF050000`–`0xFFFE0000` | 5–254 | Reserved for future default devices |
| `0xFFFF0000` | 255 | System control |

Every slot is `MmioBus::SlotSize` (0x10000 = 64 KiB) wide, computed as `MmioBus::slot(index)` —
`default_mmio::Terminal`, `default_mmio::Timer`, and so on, in
[`mmio_bus.h`](../Ceres/libs/vm/include/ceres/vm/mmio_bus.h).

## Behaviour of an unattached slot

- **Reading** an unattached slot returns all-ones: `0xFF` for a byte, `0xFFFF` for a halfword,
  `0xFFFFFFFF` for a word — the same convention an unattached port used.
- **Writing** to an unattached slot is silently discarded — no fault, no effect.

## Register layout

Every device follows the same convention: scalar registers sit at low, word-aligned offsets (`0x00`,
`0x04`, `0x08`, ...) inside its slot. A device that also moves blocks of memory — the disk, the
framebuffer, the terminal's bulk output — additionally claims three registers near the top of the
slot:

| Offset | Register | Direction | Meaning |
| --- | --- | --- | --- |
| `0xF0` | `BLOCK_ADDR` | Write | The RAM address the transfer reads from or writes to. |
| `0xF4` | `BLOCK_LEN` | Write | How many bytes to move. |
| `0xF8` | `BLOCK_CMD` | Write | `1` triggers a read (device → RAM), `2` a write (RAM → device). |

This is the direct replacement for what `inm`/`outm` used to do in a single instruction: write the
address, write the length, then write the command to fire the transfer.

## Devices implemented today

### `SystemControlDevice` (`0xFFFF0000`, write-only)

The only way to stop the VM cleanly. There is no `exit`/`quit` instruction — a program terminates by
writing a command byte to its one register:

| Value written | Effect |
| --- | --- |
| `0x01` | Shuts the machine down (invokes the shutdown callback the host registered — `ceres run` uses this to stop its run loop). |
| `0x02` | Resets the machine (invokes the reset callback). |
| anything else | Ignored. |

```casm
li  r0, 0x01
la  r13, 0xFFFF0000
strb [r13 + 0], r0    // halt the VM cleanly
```

Reading from this register returns all-ones like any other write-only device would if read.

### `TimerDevice` (`0xFF010000`)

Gives the machine its only source of asynchronous interrupts — without it, `halt` would suspend the
machine forever, since nothing could ever wake it back up.

| Offset | Register | Direction | Meaning |
| --- | --- | --- | --- |
| `0x00` | `TicksRegister` | Read | Number of instructions executed so far (a `u64` counter, truncated to 32 bits on read). |
| `0x04` | `ClockRegister` | Read | Wall-clock seconds since the Unix epoch. **This is the one value in the entire VM that is not deterministic** — everything else (including the tick count) behaves identically on every run. |
| `0x08` | `CommandRegister` | Write | Arms or disarms the timer. |

Writing to the command register:

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
li   r1, 1000
la   r13, 0xFF010008   // Timer's CommandRegister
str  [r13 + 0], r1     // fire in 1000 executed instructions
sti                     // user interrupts must be unmasked, or the timer's interrupt is lost
halt                    // suspended until the timer (or any other interrupt) fires
```

### `TerminalDevice` (`0xFF000000`)

A minimal character terminal.

| Offset | Register | Direction | Meaning |
| --- | --- | --- | --- |
| `0x00` | `StatusRegister` | Read | Bit 0 (`0x01`) set when input is available to read; bit 1 (`0x02`) is always set (the terminal is always ready to accept output in this simple implementation). |
| `0x04` | `OutputRegister` | Write | Writes a byte (or more, via the halfword/word/block forms) straight to the process's standard output as characters. |
| `0x08` | `InputRegister` | Read | Reads the next byte from a small 64-byte input ring buffer (`pushInput()`, called by the host embedding the VM), or `0` if nothing is buffered. |
| `0xF0`/`0xF4`/`0xF8` | Block registers | Write | `1` reads from the input ring into RAM; `2` writes RAM out as characters. |

```casm
// print one character
li  r0, 'H'
la  r13, 0xFF000004
strb [r13 + 0], r0

// print the null-terminated string pointed to by r1
.print_loop:
    ldrb r2, [r1]
    cmp r2, 0
    jz .print_end
    strb [r13 + 0], r2   // r13 is still the OutputRegister's address
    add r1, r1, 1
    jp .print_loop
.print_end:
```

Each `pushInput()` call that actually adds a byte to the ring buffer also raises `UserInterrupt1` —
so a program need not poll `StatusRegister` in a busy loop to notice input; it can `sti`/`halt` instead
and be woken the instant a byte arrives, the same wake-up pattern the timer uses above. See
[Interrupts and exceptions](08-Interrupts-and-Exceptions.md) and
[Interrupt vector binding](26-Interrupt-Vector-Binding.md) for how a program installs a handler for
it rather than falling through to the BIOS's default one.

The block registers work here too: writing a RAM address to `BLOCK_ADDR`, a length to `BLOCK_LEN`,
and `2` to `BLOCK_CMD` prints that many bytes in three word-stores instead of a byte-by-byte loop —
this is exactly what [`examples/main.casm`](../Ceres/examples/main.casm)'s `print` routine does.

### `DiskDevice` (`0xFF020000`)

Block storage in sectors of 512 bytes, in
[`storage_devices.h`](../Ceres/libs/devices/include/ceres/devices/storage_devices.h). One sector moves at a time: the
sector register selects which, and the block registers move it.

| Offset | Register | Direction | Meaning |
| --- | --- | --- | --- |
| `0x00` | `StatusRegister` | Read | Bit 0 `READY`, bit 1 `ERROR`. Set by whatever the last operation did. |
| `0x04` | `CommandRegister` | Write | `1` flushes to the host file. Anything else is ignored. |
| `0x08` | `SectorRegister` | Read/write | Which sector the next transfer uses. Selecting one past the end of the disk sets `ERROR`. |
| `0xF0`/`0xF4`/`0xF8` | Block registers | Write | `1` reads the selected sector into RAM, `2` writes RAM into it. |

```casm
li   r1, 3
la   r13, 0xFF020008   // Disk's SectorRegister
str  [r13 + 0], r1      // which sector
la   r13, 0xFF0200F0    // Disk's BLOCK_ADDR
str  [r13 + 0], r_buf
la   r13, 0xFF0200F4     // BLOCK_LEN
li   r2, 512
str  [r13 + 0], r2
la   r13, 0xFF0200F8      // BLOCK_CMD
li   r3, 1                 // read sector -> buffer
str  [r13 + 0], r3
```

A transfer larger than a sector is refused rather than spilling into the next one, and a shorter
one moves only what it asked for: the sector is the unit of the device, not of every transfer.

Without `--disk` the disk is still there — the registers answer and sectors keep what was written —
but the contents live only as long as the machine does. `ceres run program.casm --disk image.img`
puts a host file behind it, creating it if it is not there. Writing `1` to `CommandRegister` writes it
out, and so does the device going away when the run ends, so a program that forgets to flush still
keeps its data.

### `FramebufferDevice` (`0xFF030000`)

A grid of characters that a program draws into and then shows. Not pixels: this machine has no
window to put them in, and a grid redrawn whole is what a game on this VM actually wants — the
terminal's own output is a stream that only ever moves forward.

| Offset | Register | Direction | Meaning |
| --- | --- | --- | --- |
| `0x00` | `CommandRegister` | Write | `1` clears the grid and rewinds the write cursor; `2` shows the frame. |
| `0x04` | `WidthRegister` | Read/write | Columns, up to 200. Zero or more than that is ignored as a typo. |
| `0x08` | `HeightRegister` | Read/write | Rows, up to 100. Resizing clears the grid. |
| `0x0C` | `DataRegister` | Write | One cell per word write, continuing from where the last write left off. |
| `0xF0`/`0xF4`/`0xF8` | Block registers | Write (write only) | `2` writes a run of cells from RAM in one trigger. |

```casm
li   r1, 20
la   r13, 0xFF030004   // GPU_WIDTH
str  [r13 + 0], r1
li   r1, 10
la   r13, 0xFF030008    // GPU_HEIGHT
str  [r13 + 0], r1
li   r1, 1
la   r13, 0xFF030000     // GPU_CMD
str  [r13 + 0], r1        // clear
// ... write BLOCK_ADDR/BLOCK_LEN, then 2 to BLOCK_CMD (0xFF0300F8) to blit the whole grid ...
li   r1, 2
str  [r13 + 0], r1         // show it
```

A cell holds one printable ASCII byte; anything below `0x20` or above `0x7E` is shown as a space,
so a stray control byte cannot move the host terminal's own cursor. A presented frame goes to
stdout by default, and a host that would rather route it elsewhere — an editor, a test —
installs a sink with `setPresentSink`.

### `DmaController` (`0xFF040000`)

A real DMA engine — the successor to the pseudo-DMA `inm`/`outm` used to be. It moves memory to
memory directly, without a program copying it word by word.

| Offset | Register | Direction | Meaning |
| --- | --- | --- | --- |
| `0x00` | `SourceRegister` | Write | Physical source address. |
| `0x04` | `DestinationRegister` | Write | Physical destination address. |
| `0x08` | `LengthRegister` | Write | Bytes to move. |
| `0x0C` | `CommandRegister` | Write | `1` arms the transfer. |
| `0x10` | `StatusRegister` | Read | Bit 0 `BUSY`, bit 1 `DONE`. |

The copy does not happen on the same instruction that arms it: it runs on the controller's next
`tick()` — one instruction later — the same one-step delay `TimerDevice` already models, so a program
waiting on the completion interrupt (`UserInterrupt2`) always sees a real handoff rather than an
already-finished copy. A program that would rather poll reads `StatusRegister` instead.

```casm
la   r13, 0xFF040000   // DMA's SourceRegister
str  [r13 + 0], r_src
la   r13, 0xFF040004    // DestinationRegister
str  [r13 + 0], r_dst
la   r13, 0xFF040008     // LengthRegister
li   r1, 256
str  [r13 + 0], r1
la   r13, 0xFF04000C      // CommandRegister
li   r1, 1
str  [r13 + 0], r1          // arm it - the copy lands on the next tick
```

Because both endpoints are physical addresses in the same space, a device that later exposes its own
backing buffer as an MMIO aperture (rather than through block registers) can be a DMA source or
destination too — RAM↔RAM, RAM↔device, or device↔device all become the same operation.

## Related pages

- [Instruction set](05-Instruction-Set.md) — `ldr`/`str` and the rest; there is no dedicated I/O family any more.
- [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — how the timer's, the terminal's and the DMA controller's interrupts reach your handler.
- [Virtual memory and paging](27-Virtual-Memory-and-Paging.md) — a device's MMIO window composes with paging for free: mapping it into a program's virtual space is an ordinary page table entry.
- [Known limitations](19-Known-Limitations.md) — which devices are still stubs.

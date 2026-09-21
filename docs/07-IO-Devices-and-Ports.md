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
| `0xFF050000` | 5 | Keyboard |
| `0xFF060000` | 6 | Mouse |
| `0xFF070000` | 7 | Display (pixel framebuffer) |
| `0xFF080000` | 8 | Gamepad |
| `0xFF090000` | 9 | Audio (tone generator) |
| `0xFF0A0000`–`0xFFFE0000` | 10–254 | Reserved for future default devices |
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

A transfer whose `BLOCK_ADDR`/`BLOCK_LEN` runs past the end of RAM is **clamped** rather than
rejected: it moves what fits, and a device with a count register (the terminal's
`BlockReadCountRegister`, the DMA controller's `TransferredRegister`) reports the shortfall. An
address that points into the null page or the BIOS, or past the end of memory, moves nothing.

## Devices implemented today

### `SystemControlDevice` (`0xFFFF0000`)

The only way to stop the VM cleanly. There is no `exit`/`quit` instruction — a program terminates by
writing a command to its command register:

| Offset | Register | Direction | Meaning |
| --- | --- | --- | --- |
| `0x00` | `CommandRegister` | Write | The command in the low byte; for a shutdown, the exit status in the next byte. |
| `0x04` | `MemorySizeRegister` | Read | How many bytes of RAM the machine has. |
| `0x08` | `FeaturesRegister` | Read/write | Switches for behaviour that is off by default (below). |

| Command (low byte) | Effect |
| --- | --- |
| `0x01` | Shuts the machine down (invokes the shutdown callback the host registered — `ceres run` uses this to stop its run loop). |
| `0x02` | Resets the machine (invokes the reset callback). |
| anything else | Ignored. |

**Exit status.** A halfword or word write carries a status in bits 15:8 — `(status << 8) | 1` — and
`ceres run` exits with it as the process status. A plain byte write, which is what every program
written before this existed does, means status 0.

```casm
li  r0, 0x0701        // status 7, shut down
la  r13, 0xFFFF0000
str [r13 + 0], r0
```

**Features.** Reading `FeaturesRegister` returns what was last written (0 at reset). Bit 0,
`FeatureDivisionFault`, makes a division or modulo by zero raise the `DivisionByZero` interrupt (4)
instead of only setting the Trap flag — see [Interrupts and exceptions](08-Interrupts-and-Exceptions.md).
Every other offset, and the command register itself, reads all-ones.

```casm
li  r0, 0x01
la  r13, 0xFFFF0000
strb [r13 + 0], r0    // halt the VM cleanly
```

Reading the command register returns all-ones like any other write-only register would.

### `TimerDevice` (`0xFF010000`)

Gives the machine its only source of asynchronous interrupts — without it, `halt` would suspend the
machine forever, since nothing could ever wake it back up.

| Offset | Register | Direction | Meaning |
| --- | --- | --- | --- |
| `0x00` | `TicksRegister` | Read | Number of instructions executed so far (a `u64` counter, truncated to 32 bits on read). |
| `0x04` | `ClockRegister` | Read | Wall-clock seconds since the Unix epoch. **This is the one value in the entire VM that is not deterministic** — everything else (including the tick count) behaves identically on every run. |
| `0x08` | `CommandRegister` | Write | Arms or disarms the timer. |
| `0x0C` | `MillisRegister` | Read | Milliseconds since the machine started, from the host's steady clock (wraps after 49 days). Like `ClockRegister`, **not deterministic**: the debugger records and replays it. |

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
| `0x00` | `StatusRegister` | Read | Bit 0 (`0x01`) set when input is available to read; bit 1 (`0x02`) is always set (the terminal is always ready to accept output in this simple implementation); bit 2 (`0x04`) set at **end of input**: the host closed the input and every byte it sent has been read. |
| `0x04` | `OutputRegister` | Write | Writes a byte (or more, via the halfword/word/block forms) straight to the process's standard output as characters. |
| `0x08` | `InputRegister` | Read | Reads the next byte from a small 64-byte input ring buffer (`pushInput()`, called by the host embedding the VM), or `0` if nothing is buffered. |
| `0x0C` | `BytesAvailableRegister` | Read | How many bytes are currently buffered and unread. |
| `0x10` | `BlockReadCountRegister` | Read | How many bytes the most recent block read actually moved into RAM (a short read is how a program learns its input ended early). |
| `0x14` | `DroppedInputRegister` | Read | How many input bytes were discarded because the ring was full (truncated to 32 bits). |
| `0x18` | `ModeRegister` | Read/write | Write `1` (`ModeRaw`) to ask the host for keys as they are pressed; read what the host granted: bit 0 raw, bit 1 (`ModeKeystrokes`) the keys arrive on the keyboard's `KeyRegister`. Reads `0` when the host cannot. |
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

**Raw keys.** A console hands a program whole lines, and only once Enter is pressed: the keys before it
are held back by the console's own line editor, and an arrow or Home never arrives, because that editor
uses it. That suits `scanf`; it does not suit a menu. A program that wants each key as it is pressed writes
`ModeRaw` to `ModeRegister` and reads back what it was given:

- `ModeRaw | ModeKeystrokes` (`3`): the host took the console out of line mode, or has a window with a keyboard.
  The keys arrive on the [keyboard's](#keyboarddevice-0xff050000) `KeyRegister`, in the order they were typed,
  with no echo (the program draws what it wants shown). Writing `0` puts the console back.
- `0`: nothing changed. The input is a pipe or a file, so the program keeps reading the terminal's bytes -
  a menu can still be driven from a file that spells an arrow key the way a terminal sends it (`ESC [ A`).

The host switches the console on the thread that reads it, restores it when the machine stops (a
program that exits while raw included), and leaves Ctrl-C alone. In a window the keys already come from
the window's keyboard, so the console stays as it is and a program is granted `3` straight away. When the
program has not asked for raw keys, a window's keystrokes are also written to the terminal's input ring as
bytes (a character as UTF-8, Enter as `\n`, Backspace as `\b`, an arrow as `ESC [ A` and so on), so a program
reading its input as a stream sees what was typed; once it has asked for raw keys they are not, because they
would only pile up unread.

**End of input.** Bit 2 of the status register is what tells "no data yet" from "no data ever": it is
set only once the host has closed the input *and* the ring is empty, so a reader checks bit 0 first and
bit 2 second. `ceres run` closes the input when its standard input ends (a pipe running dry, or Ctrl-Z /
Ctrl-D at a console), and closing raises the terminal's interrupt so a program halted waiting for a byte
wakes up to see it. A host embedding the machine calls `closeInput()`. The bit never sets on a terminal
nobody closed.

`ceres run` feeds the ring from its own standard input, and does so with flow control: the reader
holds a byte back while the ring is full and pushes it when the program has taken enough, so a
piped file longer than 63 bytes arrives whole however busy the program is. (The ring keeps one slot
free, so it holds at most 63 bytes.) Only a host that calls `pushInput()` itself — a debugger, an
embedding — can overflow it, and for that source dropping is still the behaviour:
`DroppedInputRegister` counts what was lost. When the program ends, a reader still waiting for room
gives up.

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
| `0x0C` | `SectorCountRegister` | Read | How many sectors the disk has (a 64-sector default without `--disk`; the image's size with one). |
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
so a stray control byte cannot move the host terminal's own cursor.

**Colour.** Each cell also has an attribute byte. A word written to `DataRegister` holds the character
in bits 7:0 and the attribute in bits 15:8, so one store sets both. Attribute `0` means the terminal's
own colours; otherwise the low nibble is the foreground and the high nibble the background, numbered like
the ANSI palette — 0 black, 1 red, 2 green, 3 yellow, 4 blue, 5 magenta, 6 cyan, 7 white, and 8–15 the
bright versions. A frame with any coloured cell is presented with SGR escape sequences (each row ends back
on the default colours); a frame with none is the plain text it always was. Block command `3` writes a
run of attribute bytes, one per cell in the same row-major order, with a cursor of its own; clearing the
grid resets the attributes too. A presented frame goes to
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
| `0x14` | `TransferredRegister` | Read | How many bytes the last completed transfer actually moved. RAM-to-RAM it is always the length; a future device source that yields fewer bytes would report less here. |

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

### `KeyboardDevice` (`0xFF050000`)

A keyboard, distinct from the terminal: the terminal delivers a stream of characters with no notion
of which key produced them, while the keyboard reports *events* — a code plus a pressed/released
flag — so a game can tell a held key from a freshly pressed one, or stop an action on a release.

| Offset | Register | Direction | Meaning |
| --- | --- | --- | --- |
| `0x00` | `StatusRegister` | Read | Bit 0 (`0x01`) set when an event is available; bit 1 (`0x02`) set when a typed character is waiting; bit 2 (`0x04`) set when a keystroke is waiting. |
| `0x04` | `EventRegister` | Read | Pops one event: bits 30:0 = key code, bit 31 = `1` if pressed, `0` if released. `0` when empty. |
| `0x08` | `TextRegister` | Read | Pops one typed character as a Unicode code point. `0` when empty. |
| `0x0C` | `KeyRegister` | Read | Pops the next **keystroke**, in typing order. A character is its code point; a key with no character (Enter, Escape, Backspace, Tab, the arrows, Home/End, PageUp/PageDown, Insert/Delete, F1-F12) is `0x80000000 \| scancode`. `0` when empty. |
| `0x10` | `BlockReadCountRegister` | Read | How many events the last block read drained. |
| `0xF0`/`0xF4`/`0xF8` | Block registers | Write | `1` drains the event queue into RAM as a run of four-byte entries (one 32-bit event each, little-endian). |

**Typed text.** The key events say which physical key moved (a scancode); they do not say what the
layout made of it — capitals, accents, dead keys, an input method. A separate 64-entry queue carries that:
the host calls `pushText(codePoint)` (or `pushText(utf8)`, which decodes and skips malformed bytes) and a
program pops code points from `TextRegister`. Pushing text raises the same interrupt as a key event, and a
full queue drops the character. `ceres run --window` feeds it from SDL's text input.

**Keystrokes.** The event queue and the text queue are separate, so a program reading both cannot tell
whether the `a` or the Enter came first, and Enter, Escape and the arrows type no text at all. The
`KeyRegister` merges them into one ordered stream: the device itself queues each typed character, and each
*press* of a named key (a release, and a key that types a character, add nothing here), so any host that
feeds `pushText` and `pushKey` gets it - a window, a raw console, an embedding, a test. It holds 64 entries
like the others and raises the same interrupt. This is what a menu or a text field reads.

Events queue up in a 64-entry ring, exactly like the terminal's input. The host feeds the device one
event at a time with `pushKey(code, pressed)` — the code is whatever the host maps a physical key to
(a scan code, or the ASCII value of a character), kept in the low 31 bits so an SDL scancode fits —
and each push that actually lands an event raises `UserInterrupt3` (interrupt 19). A full queue drops
events; the host can read the count with `droppedEvents()`.

```casm
la   r13, 0xFF050000   // Keyboard's base
.loop:
    ldr  r1, [r13 + 0]  // StatusRegister - bit 0 set when a key event is waiting
    and  r1, r1, 1
    jz   .loop
    ldr  r1, [r13 + 4]  // EventRegister - bits 30:0 are the code, bit 31 is pressed/released
```

### `MouseDevice` (`0xFF060000`)

A mouse reporting movement two ways at once: a delta since the program last read it (what a game
wants frame to frame) and an absolute position accumulated from every motion (what an editor wants).

| Offset | Register | Direction | Meaning |
| --- | --- | --- | --- |
| `0x00` | `StatusRegister` | Read | Bit 0 set when new motion/button data has arrived since the last status read; reading it clears the flag. |
| `0x04` | `DeltaXRegister` | Read | Signed movement in X since the last read (consumed on read). |
| `0x08` | `DeltaYRegister` | Read | Signed movement in Y since the last read (consumed on read). |
| `0x0C` | `XRegister` | Read | Absolute X, accumulated from every motion. |
| `0x10` | `YRegister` | Read | Absolute Y. |
| `0x14` | `ButtonsRegister` | Read | Button mask: bit 0 left, bit 1 right, bit 2 middle. |
| `0x18` | `WheelRegister` | Read | Signed wheel movement since the last read (consumed on read). |

The host reports movement with `pushMotion(dx, dy, buttons, wheel)`; deltas and the wheel accumulate
until read, and each push that changes state raises `UserInterrupt4` (interrupt 20). Absolute
position and buttons are plain state and never reset.

### `DisplayDevice` (`0xFF070000`)

A pixel framebuffer, the display half of the "consola retro" the roadmap wants: a grid of RGB32
pixels (`0x00RRGGBB`, top byte ignored) that a program draws into and then presents. It is a
different surface from the text `FramebufferDevice` above, not a replacement — text goes to the
framebuffer, pixels to the display.

| Offset | Register | Direction | Meaning |
| --- | --- | --- | --- |
| `0x00` | `CommandRegister` | Write | `1` clears the surface to black and rewinds the cursor; `2` presents the frame. |
| `0x04` | `WidthRegister` | Read/write | Pixel columns, up to 1280. Zero or more than that is ignored as a typo. |
| `0x08` | `HeightRegister` | Read/write | Pixel rows, up to 720. Resizing clears the surface. |
| `0x0C` | `DataRegister` | Write | One pixel per word write, continuing from where the last write left off. |
| `0xF0`/`0xF4`/`0xF8` | Block registers | Write (write only) | `2` blits a run of pixels from RAM in one trigger; `BLOCK_LEN` is bytes (a multiple of 4). |

The host routes a presented frame with `setFrameSink(width, height, pixels)`; without a sink nothing
happens, so a headless build stays silent and `ceres run --window` uploads the pixels into an SDL
texture instead (see the [SDL3 plan](29-SDL3-Integration-Plan.md)).

```casm
li   r1, 320
la   r13, 0xFF070004   // DISP_WIDTH
str  [r13 + 0], r1
li   r1, 200
la   r13, 0xFF070008   // DISP_HEIGHT
str  [r13 + 0], r1
// ... write BLOCK_ADDR/BLOCK_LEN, then 2 to BLOCK_CMD (0xFF0700F8) to blit the pixels ...
li   r1, 2
la   r13, 0xFF070000   // DISP_CMD
str  [r13 + 0], r1     // present
```

### `GamepadDevice` (`0xFF080000`)

A gamepad, **polled rather than event-driven**: a game loop reads the button mask and the axes every
frame instead of draining a queue. The host reports the whole current state with
`pushState(...)`, and a state that actually changed raises `UserInterrupt5` (interrupt 21) — so a
program can either poll the registers or `sti`/`halt` and wake on input.

| Offset | Register | Direction | Meaning |
| --- | --- | --- | --- |
| `0x00` | `StatusRegister` | Read | Bit 0 set when the state changed since the last status read; reading it clears the flag. |
| `0x04` | `ButtonsRegister` | Read | Button bitmask: bit 0 south (A), 1 east (B), 2 west (X), 3 north (Y), 4 back, 5 guide, 6 start, 7/8 stick presses, 9/10 shoulders, 11–14 the D-pad. |
| `0x08`/`0x0C` | `LeftXRegister` / `LeftYRegister` | Read | Signed left stick (−32768…32767). |
| `0x10`/`0x14` | `RightXRegister` / `RightYRegister` | Read | Signed right stick. |
| `0x18`/`0x1C` | `LeftTriggerRegister` / `RightTriggerRegister` | Read | Triggers (0…32767). |

```casm
la   r13, 0xFF080000   // Gamepad's base
.loop:
    ldr  r1, [r13 + 4]  // ButtonsRegister - bit 0 is the south/A button
    and  r1, r1, 1
    jz   .loop          // wait until A is held
```

### `AudioDevice` (`0xFF090000`)

A tone generator: one voice, a note at a time. Not a sample player — a beeper with a choice of timbre,
the audio half of the retro console. The device holds what the program asked for; a host with speakers
installs a sink (`setToneSink`) that plays it, and `ceres run --window` does so through SDL. Without a
host to play it the machine is silent and never busy.

| Offset | Register | Direction | Meaning |
| --- | --- | --- | --- |
| `0x00` | `StatusRegister` | Read | Bit 0 set while a tone is playing. |
| `0x04` | `FrequencyRegister` | Read/write | Hertz, clamped to 20–20000. |
| `0x08` | `DurationRegister` | Read/write | Milliseconds; `0` plays until stopped. |
| `0x0C` | `VolumeRegister` | Read/write | 0–255. |
| `0x10` | `WaveformRegister` | Read/write | 0 square, 1 triangle, 2 sawtooth, 3 sine, 4 noise. Any other value is ignored. |
| `0x14` | `CommandRegister` | Write | `1` plays a tone with the registers above (replacing one already playing), `2` stops. |

When a tone runs its whole duration the host reports it (`toneFinished()`): the busy bit clears and
`UserInterrupt6` (22) is raised, so a program can wait with `sti`/`halt`. A tone that is stopped or
replaced raises nothing. An unattached slot reads all-ones, which is how a program tells this machine has
no audio at all.

```casm
la   r13, 0xFF090000
li   r1, 440
str  [r13 + 4], r1     // frequency
li   r1, 200
str  [r13 + 8], r1     // 200 ms
li   r1, 1
str  [r13 + 0x14], r1  // play
```

## Related pages

- [Instruction set](05-Instruction-Set.md) — `ldr`/`str` and the rest; there is no dedicated I/O family any more.
- [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — how the timer's, the terminal's and the DMA controller's interrupts reach your handler.
- [Virtual memory and paging](27-Virtual-Memory-and-Paging.md) — a device's MMIO window composes with paging for free: mapping it into a program's virtual space is an ordinary page table entry.
- [Known limitations](19-Known-Limitations.md) — which devices are still stubs.

# Video: the GPU, levels V0 and V1

[← Back to index](README.md)

The machine's screen is a simulated GPU at `0xFF400000` (slot `0x40`, plan/v2 SPEC 7). It reads what it shows
from the **VRAM** at `0xA0000000` (see [Memory](02-Memory.md#the-physical-map)) and composes it once a frame,
at the vertical blank; the program never draws on the window itself. The GPU has **levels**: V0 is a text
terminal and V1 adds a bitmap and a copy engine. V2–V6 (tiles and sprites, a 2D engine, vectors, 3D) are in the
[v2 plan](../plan/v2/README.md) and not implemented yet: `Caps` says which levels there are.

The terminal of [the virtual terminal](33-Terminal-and-Debug-Log.md) draws on the GPU's text plane, so a program
that only prints already uses V0 without knowing it.

The source is in [`libs/devices/src/video`](../Ceres/libs/devices/src/video): `gpu.cpp` (the registers and the
frame timing), `text_plane.cpp`, `bitmap_plane.cpp`, `copy_engine.cpp`, and `software_executor.cpp` (the scanout,
which turns the planes into pixels). The standard library wraps it in `ceres/video.h`, `ceres/text.h` and
`ceres/fb.h`.

## The core registers

| Offset | Register | Access | Meaning |
| --- | --- | --- | --- |
| `0x000` | `Id` | R | `0x55504743` (`"CGPU"`). |
| `0x004` | `Version` | R | Major << 16 \| minor: `0x00010000`. |
| `0x008` | `Caps` | R | Bits 0–6: the levels available (bit n, level Vn). Bits 8–15: the bitmap formats (all eight from V1). Bit 31: a hardware executor (never, so far). |
| `0x00C` | `VramSize` | R | The VRAM, in bytes (the profile's, or `--vram`). |
| `0x010` | `GpuClockHz` | R | The GPU clock (the profile's, or `--gpu-clock`); what the copy engine's cost is counted in. |
| `0x014` | `Mode` | RW | The level in use, 0–6. A write above what the machine has is clamped to it. Starts at 0. |
| `0x018` | `MaxLevel` | R | The highest level the profile allows (`--max-video`). |
| `0x01C` | `Control` | RW | Bit 0 display on (set at start: off, the screen is black); bit 1 command processor (V3, no effect yet); bit 2 writes as a reset of the whole GPU. |
| `0x020` | `Status` | R | Bit 0 the copy engine is busy; bit 1 a fault is pending; bit 2 the scan is in the vertical blank; bit 3 a `Present` waits for the blank. |
| `0x024` | `IrqEnable` | RW | Bit 0 VBlank (interrupt 32), bit 1 line (33), bit 2 copy done (34), bit 3 fault (35). |
| `0x028` | `IrqStatus` | W1C | What happened, by the same bits, whether enabled or not; writing 1s clears them (bit 3 clears the fault too). |
| `0x02C` | `FaultCode` | R | `1`: an engine was given an address outside RAM and VRAM. `0`: no fault. |
| `0x030` | `FaultAddress` | R | The address that faulted. |
| `0x100` | `Width` | RW | The screen in pixels, at least 8 and at most the profile's largest (`--max-resolution`). |
| `0x104` | `Height` | RW | At least 16. Changing either resizes the text plane (`Width / 8` by `Height / 16` cells). |
| `0x108` | `Refresh` | R | Frames a second: 60, or 50 with `--refresh 50`. |
| `0x10C` | `LinesTotal` | R | Lines a frame: `Height + 45` of blanking, at least 262. |
| `0x110` | `VCount` | R | The line being scanned now; `Height` and beyond is the blank. |
| `0x114` | `LineCompare` | RW | With `IrqEnable` bit 1, interrupt 33 when the scan reaches this line. |
| `0x118` | `FrameCounter` | R | Vertical blanks since the start (32 bits, wraps). |
| `0x11C` | `BackgroundColor` | RW | `0x00RRGGBB`, behind every plane. |
| `0x120` | `Present` | W | `1`: apply the pending bases and flip at the next vertical blank. |

The machine starts at 640 x 480 (or the profile's largest, if it is smaller), in V0, with the display on, the
text plane shown and nothing else.

## Time: frames and the vertical blank

The GPU's time is the machine's (see [The machine's clock](30-Machine-Clock-and-Profiles.md)): frame *n* starts at
cycle *n* · `CpuClockHz` / `Refresh` after the start, and a frame is `LinesTotal` lines of equal length. The
vertical blank begins when the scan passes the last visible line (`VCount == Height`) and lasts to the end of the
frame. It is an event of the scheduler, so a program in `halt` wakes for it and `--speed max` skips straight to it.

At every vertical blank the GPU:

1. applies what a `Present` asked for (the bases of the planes, the bitmap's flip), if one did;
2. counts the frame and sets `IrqStatus` bit 0, raising interrupt 32 if it is enabled;
3. tells the host a frame is ready, and the host shows it (see [Where the frames go](#where-the-frames-go)).

A program that wants one picture a frame draws into the back buffer, writes `1` to `Present` and waits for the
blank - `halt` with interrupt 32 enabled, or `FrameCounter` changing. Writing twice before a blank presents once.

## VRAM at start

The GPU fills the VRAM as the machine starts (SPEC 7.4), from `0xA0000000`, each part at a multiple of 256:

| What | Size | Register that says where |
| --- | --- | --- |
| The text cells of the largest screen the profile has | `cols * rows * 2` | `CellsBase` |
| The 8 x 16 font, 256 glyphs | 4 KiB | `FontBase` |
| The palette, 256 entries of `0x00RRGGBB` | 1 KiB | `TextPaletteBase`, and the bitmap's `PaletteBase` |
| The scrollback ring | a quarter of the VRAM, at most 256 KiB | `ScrollbackBase`, `ScrollbackLines` |
| Three bitmap buffers of the screen's size (XRGB8888) | as far as the VRAM holds them | `Base`, `BackBase`, `SpareBase` |

A program reads these addresses from the registers; it never assumes them, since they move with the profile. The
rest of the VRAM is the program's.

The palette's first 16 entries are the ANSI colours (VGA's: black, red, green, brown, blue, magenta, cyan, light
grey, then the bright eight); 16–231 are xterm's 6 x 6 x 6 cube and 232–255 its 24 greys.

## V0: the text plane (`0x200`–`0x23C`)

A grid of 8 x 16 cells over everything else.

| Offset | Register | Meaning |
| --- | --- | --- |
| `0x200` | `TextEnable` | `1` (the default) shows the plane. |
| `0x204`, `0x208` | `TextCols`, `TextRows` (R) | `Width / 8` and `Height / 16`. |
| `0x20C` | `CellsBase` | The first cell, row-major, in VRAM. |
| `0x210` | `CellFormat` | `0` 16-bit cells (the default), `1` 32-bit. |
| `0x214` | `FontBase` | The font: 16 bytes a glyph, bit 7 the left pixel. |
| `0x218` | `GlyphCount` | 1–256; a glyph past it draws blank. |
| `0x21C` | `TextPaletteBase` | 256 entries of `0x00RRGGBB`. |
| `0x220`, `0x224` | `CursorX`, `CursorY` | Where the cursor is drawn. |
| `0x228` | `CursorShape` | Bits 1:0 none, underline, block or bar; bit 8 blinks (16 frames on, 16 off). |
| `0x22C`, `0x230` | `ScrollbackBase`, `ScrollbackLines` | The ring the terminal keeps the rows that scrolled off in, and how many it holds. |
| `0x234` | `ScrollY` | Rows of the ring shown above the screen; `0` shows the live screen (and the cursor). |
| `0x238`, `0x23C` | `ScrollbackHead`, `ScrollbackCount` | The ring's next row and how many rows it holds now. |

`CellsBase`, `FontBase` and `TextPaletteBase` take effect at the next `Present`, so a program can build a new
screen elsewhere and switch to it at once.

**Cells.** A 16-bit cell is the glyph in bits 7:0, the ink in 11:8 and the background in 15:12 (the palette's
first 16 entries). A 32-bit cell is the glyph in 7:0, the ink in 15:8 and the background in 23:16 (all 256),
bit 24 underlines and bit 25 swaps ink and background. In both, **background 0 is transparent**: the bitmap
plane or the background colour shows through, which is how text goes over a picture.

**The font.** Glyphs `0x20`–`0x7E` and `0xA0`–`0xFF` are Latin-1 (the accented letters of Spanish, French,
German, Portuguese, `¿`, `¡`, `°`, `£`…); `0x80`–`0x9F`, which are controls in Latin-1, hold the box-drawing and
block characters (`─│┌┐└┘├┤┬┴┼═║╔╗╚╝╠╣╦╩╬█▀▄░▒▓■•▲▼`, in that order). The terminal translates those code points
to them, so `printf("┌──┐")` draws a box. A program may load its own font at `FontBase`.

```casm
// "Hi" in yellow on blue at the top left, through the text plane's own registers
la   r13, 0xFF400000
ldr  r1, [r13 + 0x20C]      // CellsBase
li   r2, 0x1E48             // 'H', ink 14 (yellow), background 1 (blue)
strh [r1 + 0], r2
li   r2, 0x1E69             // 'i'
strh [r1 + 2], r2
```

## V1: the bitmap plane (`0x240`–`0x26C`)

A picture behind the text, shown when `Mode` is 1 or more and `BitmapEnable` is set.

| Offset | Register | Meaning |
| --- | --- | --- |
| `0x240` | `BitmapEnable` | `1` shows the plane. |
| `0x244` | `Base` | The picture shown; a write takes effect at the next `Present`. |
| `0x248` | `BackBase` | The picture to draw into. |
| `0x24C` | `Pitch` | Bytes from a row to the next (the screen's width times 4 at start). |
| `0x250` | `Format` | `0` I1, `1` I2, `2` I4, `3` I8, `4` RGB565, `5` ARGB1555, `6` XRGB8888 (the default), `7` ARGB8888. Any other value is ignored. |
| `0x254`, `0x258` | `BitmapWidth`, `BitmapHeight` | The picture's size, which may differ from the screen's. |
| `0x25C`, `0x260` | `ScrollX`, `BitmapScrollY` | The picture's pixel at the screen's top left; the picture wraps round. |
| `0x264` | `Buffers` | 1–3 (below). |
| `0x268` | `PaletteBase` | The palette of the indexed formats; a write takes effect at the next `Present`. |
| `0x26C` | `SpareBase` | The third picture, with `Buffers` 3. |

**Formats.** The indexed ones (I1, I2, I4, I8) look each pixel up in the palette; the sub-byte ones pack the
leftmost pixel in the byte's high bits. RGB565 and ARGB1555 are 16 bits a pixel, little-endian; XRGB8888 and
ARGB8888 are `0xAARRGGBB` words. In ARGB1555 a pixel with its alpha bit clear is transparent, and ARGB8888 blends
its alpha over the background colour. A picture smaller than the screen repeats across it, as it wraps round.

**Buffers.** With 1, `Base` is the picture shown and a program draws on it (tearing, if it races the scan). With
2, a `Present` swaps `Base` and `BackBase` at the blank: the program draws into `BackBase`, presents, and draws the
next frame into what `BackBase` then is. With 3, a `Present` queues `BackBase` to be shown and makes `SpareBase` the
new back buffer, so the program never waits for the blank; the blank shows the queued picture and the one it
replaces becomes the spare.

## V1: the copy engine (`0x280`–`0x294`)

Moves or fills memory without the CPU: in RAM, in VRAM, or between them.

| Offset | Register | Meaning |
| --- | --- | --- |
| `0x280` | `CopySrc` | Where a copy reads. |
| `0x284` | `CopyDst` | Where a copy or a fill writes. |
| `0x288` | `CopyLength` | Bytes. |
| `0x28C` | `FillValue` | The 32-bit pattern a fill repeats, aligned to the address (a fill that starts at an odd byte starts mid-pattern). |
| `0x290` | `CopyCommand` | `1` copy, `2` fill. |
| `0x294` | `CopyStatus` | Bit 0 busy; bit 1 the last command faulted. |

The bytes move when the command is written, but the engine is **busy** for 16 GPU cycles plus one for every 4 bytes
of a copy (8 of a fill), turned into CPU cycles at the two clocks and rounded up; then `IrqStatus` bit 2 is set and
interrupt 34 raised if enabled. A command given while it is busy waits for none: it is ignored. A source or
destination range that leaves RAM and VRAM does nothing, sets `FaultCode` 1 and `FaultAddress`, and raises
interrupt 35 if enabled.

```casm
// clear a 640 x 480 XRGB8888 back buffer to dark blue
la   r13, 0xFF400000
ldr  r1, [r13 + 0x248]      // BackBase
str  [r13 + 0x284], r1      // CopyDst
li   r2, 1228800            // 640 * 480 * 4
str  [r13 + 0x288], r2      // CopyLength
li   r2, 0x00000040
str  [r13 + 0x28C], r2      // FillValue
li   r2, 2
str  [r13 + 0x290], r2      // fill
```

The blitter at `0xFF460000` (see [I/O devices](07-IO-Devices-and-Ports.md#blitterdevice-0xff460000)) also draws
into VRAM surfaces, until the 2D engine of V3 replaces it.

## Where the frames go

The GPU composes a frame - the background colour, then the bitmap plane, then the text plane with its cursor - and
hands it to the host at the blank. The host decides what to do with it:

- **In the window** (`ceres run`, in a build with SDL): the window opens at the start, shows the latest frame at
  the largest whole scale that fits (F11 or `--fullscreen` fills the screen), and at most one frame every 8 ms of
  the host's time is drawn. Its title is the status bar: the profile, the speed and, once the program ends, its
  exit code. The window stays with the last frame until a key is pressed or it is closed, unless
  `--exit-on-halt`.
- **Without one** (`--headless`, `CERES_HEADLESS`, or a build without SDL): nothing is drawn. `--frames <dir>`
  writes a PNG of the screen for every `Present` (`frame_000000.png`, …), and `--screen-log <file>` writes the
  text plane as text at every `Present` and at the end - how the tests check what a program shows.

Nothing about the program's screen ever reaches the host's terminal (SPEC 1.4); see
[CLI](16-CLI-and-Assembly-Pipeline.md) for the options.

## Related pages

- [The virtual terminal and the debug log](33-Terminal-and-Debug-Log.md) — what draws on the text plane.
- [I/O devices and MMIO](07-IO-Devices-and-Ports.md) — the other devices and the address map.
- [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — interrupts 32–35.
- [The machine's clock and its profiles](30-Machine-Clock-and-Profiles.md) — the clocks, the VRAM and the largest
  screen of each profile.

# Video: the GPU, levels V0 to V2

[← Back to index](README.md)

The machine's screen is a simulated GPU at `0xFF400000` (slot `0x40`, plan/v2 SPEC 7). It reads what it shows
from the **VRAM** at `0xA0000000` (see [Memory](02-Memory.md#the-physical-map)) and scans it line by line, once a
frame; the program never draws on the window itself. The GPU has **levels**: V0 is a text terminal, V1 adds a
bitmap and a copy engine, and V2 (Retro 2D) tile layers, an affine layer, sprites and a line table, as the 8- and
16-bit consoles had them. V3–V6 (a 2D engine, vectors, 3D) are in the [v2 plan](../plan/v2/README.md) and not
implemented yet: `Caps` says which levels there are.

The terminal of [the virtual terminal](33-Terminal-and-Debug-Log.md) draws on the GPU's text plane, so a program
that only prints already uses V0 without knowing it.

The source is in [`libs/devices/src/video`](../Ceres/libs/devices/src/video): `gpu.cpp` (the registers, the frame
timing and the scan line by line), `text_plane.cpp`, `bitmap_plane.cpp`, `copy_engine.cpp`, `retro2d.cpp` (V2's
registers), `sprites.cpp` (reading the OAM and the sprites of a line), and `software_executor.cpp` with
`retro_scanout.cpp` (the scanout, which turns the planes into pixels). The standard library wraps it in
`ceres/video.h`, `ceres/text.h`, `ceres/fb.h`, `ceres/tiles.h` and `ceres/sprite.h`.

## The core registers

| Offset | Register | Access | Meaning |
| --- | --- | --- | --- |
| `0x000` | `Id` | R | `0x55504743` (`"CGPU"`). |
| `0x004` | `Version` | R | Major << 16 \| minor: `0x00010000`. |
| `0x008` | `Caps` | R | Bits 0–6: the levels available (bit n, level Vn; V0–V2, as far as the profile allows). Bits 8–15: the bitmap formats (all eight from V1). Bit 31: a hardware executor (never, so far). |
| `0x00C` | `VramSize` | R | The VRAM, in bytes (the profile's, or `--vram`). |
| `0x010` | `GpuClockHz` | R | The GPU clock (the profile's, or `--gpu-clock`); what the copy engine's cost is counted in. |
| `0x014` | `Mode` | RW | The level in use, 0–6. A write above what the machine has is clamped to it. Starts at 0. |
| `0x018` | `MaxLevel` | R | The highest level the profile allows (`--max-video`). |
| `0x01C` | `Control` | RW | Bit 0 display on (set at start: off, the screen is black); bit 1 command processor (V3, no effect yet); bit 2 writes as a reset of the whole GPU. |
| `0x020` | `Status` | R | Bit 0 the copy engine is busy; bit 1 a fault is pending; bit 2 the scan is in the vertical blank; bit 3 a `Present` waits for the blank. |
| `0x024` | `IrqEnable` | RW | Bit 0 VBlank (interrupt 32), bit 1 line (33), bit 2 copy done (34), bit 3 fault (35). |
| `0x028` | `IrqStatus` | W1C | What happened, by the same bits, whether enabled or not; writing 1s clears them (bit 3 clears the fault too). |
| `0x02C` | `FaultCode` | R | `1`: an engine was given an address outside RAM and VRAM. `2`: on `micro` or `pocket`, the CPU stored to the VRAM outside the vertical blank (the store was lost; see [below](#vram-in-the-vertical-blank-micro-and-pocket)). `0`: no fault. |
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

1. finishes the frame just scanned, and counts its sprites a line (`SpriteStatus`, V2);
2. applies what a `Present` asked for (the bases of the planes, the bitmap's flip), if one did;
3. counts the frame and sets `IrqStatus` bit 0, raising interrupt 32 if it is enabled;
4. tells the host the frame is ready, and the host shows it (see [Where the frames go](#where-the-frames-go)).

A program that wants one picture a frame draws into the back buffer, writes `1` to `Present` and waits for the
blank - `halt` with interrupt 32 enabled, or `FrameCounter` changing. Writing twice before a blank presents once.
What a `Present` applies is seen from the next frame on, the first scanned with it.

### The scan, line by line

Each visible line is composed with the registers as they were when the scan reached it (SPEC 7.6): what the CPU
writes while line *L* is on its way - in the handler of `LineCompare`'s interrupt, say - changes the picture from
line *L* + 1 (a write on the very cycle *L* starts is in time for *L*), and the line table's entries for *L* are
written before *L*. So a split screen, a sky of several colours or a floor in perspective are exact, whenever the
program does it. The GPU does not draw as it goes: it keeps the stretches of lines that share their registers and
composes the frame at its blank, when the VRAM behind them - tiles, maps, palettes, the OAM, cells - is read. The
text plane is composed as it is at that moment. Before any access to one of its registers the GPU catches up with
the machine's cycle, so a program always reads what the scan has done by then.

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

## V2: the tile layers (`0x300`–`0x3DC`)

Level V2 draws the screen from small pictures, as the consoles did: four **tile layers** and an **affine layer**,
coloured through palettes, with the sprites among them (below). Its registers take effect at once - no `Present` -
and are seen with `Mode` 2 or more. At the start, and at a reset of the GPU, they are 0, but for the affine
layer's matrix, which is the identity.

| Offset | Register | Meaning |
| --- | --- | --- |
| `0x300` | `TilePaletteBase` | The layers' palette: 256 entries of `0x00RRGGBB`; 4-bit tiles see it as 16 banks of 16. |
| `0x304` | `SpritePaletteBase` | The sprites' palette, the same way. |
| `0x308`–`0x318` | the sprites | See [V2: the sprites](#v2-the-sprites-0x308-0x318). |
| `0x31C`, `0x320` | the line table | See [V2: the line table](#v2-the-line-table-0x31c-0x320). |
| `0x340 + 0x20·n` | layer *n* (0–3) | `Control` `+0x00`, `MapBase` `+0x04`, `TileBase` `+0x08`, `ScrollX` `+0x0C`, `ScrollY` `+0x10`, `LineScrollBase` `+0x14`. |
| `0x3C0`–`0x3D8` | the affine layer | `AffineControl` `0x3C0`, `AffineMapBase` `0x3C4`, `AffineTileBase` `0x3C8`, `AffineOriginX` `0x3CC`, `AffineOriginY` `0x3D0`, `AffineMatrixAB` `0x3D4`, `AffineMatrixCD` `0x3D8`. |

`0x324`–`0x33F` and `0x3E0`–`0x3FF` are kept for V3.

A layer's **`Control`**: bit 0 on; bit 1 tiles of 16×16 (8×8 without it); bit 2 8 bits a pixel (4 without it);
bits 5:4 the priority, 0–3, 3 in front; bits 7:6 the map's width in tiles (0: 32, 1: 64, 2 and 3: 128); bits 9:8
its height, the same; bit 10 the table of a scroll a line; bit 11 the affine layer repeats its map (without it,
what is outside the map is clear); bits 15:12 the palette bank, added to the entries'.

The **map** is width × height entries of 16 bits, row by row, at `MapBase`. An entry: bits 9:0 the tile; 12:10 its
palette (at 4 bits a pixel the bank is (palette + the layer's bank) mod 16; at 8 bits it is ignored); bit 13 flips
it across, bit 14 up and down; bit 15 puts it one priority in front of its layer (3 at most).

The **tiles** are at `TileBase` + number × the tile's size (8×8: 32 bytes at 4 bits, 64 at 8; 16×16: 128 and 256),
row by row, two pixels to a byte at 4 bits with the left one in the high half (as the bitmap's I4). Colour 0 is
clear - the 0 of every bank at 4 bits - and shows what is behind.

**Scrolling.** Screen pixel (*x*, *y*) shows map pixel (*x* + `ScrollX` + *dx*, *y* + `ScrollY` + *dy*), and the map
repeats both ways. With bit 10, *dx* and *dy* are the word for line *y* at `LineScrollBase` + 4·*y* (bits 15:0 and
31:16, signed): a layer that waves, or scrolls at several speeds. Without it they are 0.

**The affine layer** draws a map the same way (the same entries and tiles) through a matrix: screen pixel (*x*,
*y*) is map pixel (*u*, *v*) with *u* = (`OriginX` + *pa*·*x* + *pb*·*y*) >> 8 and *v* = (`OriginY` + *pc*·*x* +
*pd*·*y*) >> 8. The origin is signed 24.8; *pa* is bits 15:0 of `AffineMatrixAB` and *pb* its bits 31:16, *pc* and
*pd* those of `AffineMatrixCD`, signed 8.8. The shift is arithmetic, so −0.5 is pixel −1. A turned or zoomed map,
and - writing the origin and the matrix every line from the line table - a floor in perspective.

**Priority.** From the back: the background colour, the bitmap plane, then for each priority from 0 to 3 the
layers of that priority - the affine layer, then layers 3, 2, 1 and 0 - and the sprites of that priority; the text
plane is always in front.

## V2: the sprites (`0x308`–`0x318`)

| Offset | Register | Meaning |
| --- | --- | --- |
| `0x308` | `OamBase` | The 128 sprites, 16 bytes each. |
| `0x30C` | `SpriteTileBase` | Their pictures. |
| `0x310` | `SpriteControl` | Bit 0 on. |
| `0x314` | `SpriteStatus` | R. Of the last frame, set at each vertical blank: bit 0 a line had more sprites than `SpriteLimit`, bits 31:16 the first. |
| `0x318` | `SpriteLimit` | R. Sprites a line, the profile's (16 on `micro`, 128 on `standard`). |

An entry of the **OAM**: word 0, bits 15:0 *x* and 31:16 *y*, signed, the top-left corner on the screen. Word 1:
bits 15:0 the picture, at `SpriteTileBase` + 32 × this; 19:16 the palette bank (4 bits a pixel); 21:20 the width
(8, 16, 32 or 64) and 23:22 the height; bit 24 flips across, 25 up and down; 27:26 the priority; bit 28 8 bits a
pixel; bit 31 visible. Words 2 and 3 are V3's. The picture is width × height pixels row by row, packed as the
tiles are; colour 0 is clear. Where two sprites meet, the lower number is in front, whatever their priorities.

The visible sprites that touch a line are taken in the order of their numbers, off the screen across or not; past
`SpriteLimit` the rest are left out of that line, and `SpriteStatus` says so after the frame.

## V2: the line table (`0x31C`–`0x320`)

`LineTableBase` names a table in VRAM of up to 8192 entries (`LineTableCount`, 0 off) of 8 bytes: word 0 is the
line in bits 15:0 and the offset of a register of the GPU's slot in bits 27:16, word 1 a value. At the start of each
visible line the scan writes the values of that line's entries, as the CPU would - and they stay written. The table
is gone through in order and an entry whose line has passed is skipped; only `BackgroundColor`, the bitmap's
`ScrollX` and `ScrollY` and V2's registers (not `SpriteStatus`, `SpriteLimit` or the table's own) are written, the
rest ignored. The GPU reads the whole table, with `LineTableBase` and `LineTableCount`, at the first cycle of each
frame's line 0: what changes later is for the next frame. A program usually rewrites it in the vertical blank.

```casm
// a sky of four bands: the background colour at lines 0, 60, 120 and 180
@rodata
let sky: u32[8] = [0x011C0000, 0x203080, 0x011C003C, 0x3050A0, 0x011C0078, 0x5078C0, 0x011C00B4, 0x80A0E0]
```

## VRAM in the vertical blank (`micro` and `pocket`)

On the two smallest profiles (D17) the CPU may store to the VRAM only in the vertical blank or with the display off
(`Control` bit 0 clear), as on the handhelds whose scan held the video memory while it drew. Any other store - a
plain one, or a chunk of a block instruction - is lost: the GPU sets `FaultCode` 2 and `FaultAddress`, and raises
interrupt 35 if enabled; the CPU goes on. The DMA, the copy engine and the terminal are not held to it, so a program
loads its tiles with the copy engine, or with the display off, and writes its maps and OAM in the blank. A `custom`
machine made from `micro` or `pocket` keeps the rule.

## Where the frames go

The GPU composes the frame just scanned - the background colour, then the bitmap plane, then the tile layers and
sprites, then the text plane with its cursor - and hands it to the host at the blank. The host decides what to do
with it:

- **In the window** (`ceres run`, in a build with SDL): the window opens at the start, shows the latest frame at
  the largest whole scale that fits (F11 or `--fullscreen` fills the screen), and at most one frame every 8 ms of
  the host's time is drawn. Its title is the status bar: the profile, the speed and, once the program ends, its
  exit code. The window stays with the last frame until a key is pressed or it is closed, unless
  `--exit-on-halt`.
- **Without one** (`--headless`, `CERES_HEADLESS`, or a build without SDL): nothing is drawn. `--frames <dir>`
  writes a PNG for every `Present` (`frame_000000.png`, …) - of the first frame that shows it, written when that
  frame ends (or, when the program ends first, of the screen as it is then) - and `--screen-log <file>` writes the
  text plane as text at every `Present` and at the end: how the tests check what a program shows.

Nothing about the program's screen ever reaches the host's terminal (SPEC 1.4); see
[CLI](16-CLI-and-Assembly-Pipeline.md) for the options.

## Related pages

- [The virtual terminal and the debug log](33-Terminal-and-Debug-Log.md) — what draws on the text plane.
- [I/O devices and MMIO](07-IO-Devices-and-Ports.md) — the other devices and the address map.
- [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — interrupts 32–35.
- [The machine's clock and its profiles](30-Machine-Clock-and-Profiles.md) — the clocks, the VRAM and the largest
  screen of each profile.

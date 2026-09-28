# The virtual terminal and the debug log

[← Back to index](README.md)

A program has two ways to speak: its **terminal**, for its user, and its **debug log**, for whoever runs it. Neither
is the host's terminal. The virtual terminal at `0xFF000000` is drawn on the GPU's text plane, in the machine's
window, and reads what is typed there (plan/v2 SPEC 8); the debug log at `0xFF030000` goes to the host's log, on
stderr or in a file (SPEC 5.7). What a program writes never reaches the process's stdout, with a window or without
one (SPEC 1.4) - which is what lets a test compare a program's output byte for byte, and a gate test holds every
example to it.

The source is in [`libs/devices/src/terminal`](../Ceres/libs/devices/src/terminal): `terminal.cpp` (the device and
the drawing), `ansi_parser.cpp` (UTF-8 and escape sequences), `line_discipline.cpp` (the line editor) and
`scrollback.cpp`. The standard library's `ceres/terminal.h`, `stdio.h` and `key.h` use it.

## The registers

| Offset | Register | Access | Meaning |
| --- | --- | --- | --- |
| `0x00` | `Status` | R | Bit 0 input is waiting; bit 1 ready for output (always); bit 2 end of input; bit 3 Ctrl+C pending. |
| `0x04` | `Output` | W | The low byte goes to the output: UTF-8, controls and ANSI sequences. |
| `0x08` | `Input` | R | The next input byte, or `0` when there is none. |
| `0x0C` | `Available` | R | How many input bytes are waiting. |
| `0x10` | `Mode` | RW | Bit 0 raw, bit 1 echo, bit 2 history, bit 3 interrupt 19 when input comes. Starts at `0xE`: cooked, echo, history, interrupt. |
| `0x14` | `ErrorOutput` | W | The low byte goes to the output in the error colour (the error stream). |
| `0x18` | `Control` | RW | Bit 0 on (off, nothing is drawn); bit 1 cursor visible; bit 2 keep a scrollback; bit 3 scroll at the bottom (off, the last row is written over). Starts at `0xF`. |
| `0x1C`, `0x20` | `Cols`, `Rows` | R | The screen in cells: the text plane's. |
| `0x24`, `0x28` | `CursorX`, `CursorY` | RW | Where the next character goes. |
| `0x2C` | `InterruptAck` | W | `1` clears a pending Ctrl+C (`Status` bit 3). |
| `0xF0`, `0xF4` | `BlockAddress`, `BlockLength` | W | A buffer in RAM. |
| `0xF8` | `BlockCommand` | W | `1` writes the buffer to the output, `2` reads up to `BlockLength` input bytes into it, `3` writes it to the error stream. |
| `0xFC` | `BlockCount` | R | The bytes the last block command moved: a short read is how a program learns it has all there is. |

```casm
// print "Hi\n" with one block command
la   r13, 0xFF000000
la   r1, message
str  [r13 + 0xF0], r1       // BlockAddress
li   r2, 3
str  [r13 + 0xF4], r2       // BlockLength
li   r2, 1
str  [r13 + 0xF8], r2       // write
```

## Output

The terminal draws what the program writes on the text plane (see [Video](31-Video.md#v0-the-text-plane-0x2000x23c)),
as a terminal would:

- **UTF-8.** A character becomes a glyph of the font: Latin-1 as it is, the box-drawing and block characters
  (`─│┌┐└┘├┤┬┴┼═║╔╗╚╝╠╣╦╩╬█▀▄░▒▓■•▲▼`) as the font's `0x80`–`0x9F`, and anything else as `?`. A sequence cut
  between two writes is put together.
- **Controls.** `\r` to column 0; `\n` down a row and to column 0; `\b` a column back; `\t` to the next multiple of
  8. `\a` will sound once the audio has a tone generator on the new machine (F9); until then it does nothing.
- **Wrapping.** A character in the last column leaves the cursor there, and the next one starts a new row - so
  a line exactly as wide as the screen does not leave an empty row after it.
- **Escape sequences** (`ESC [` …): `A` `B` `C` `D` move, `H` and `f` place (1-based), `J` and `K` erase (0 to
  the end, 1 from the start, 2 all), `s` and `u` save and restore the cursor (and `ESC 7`, `ESC 8`), `?25l` and
  `?25h` hide and show it, `r` sets the rows that scroll, and `m` the colours: 0, 1 (bright), 7 and 27
  (reverse), 22, 30–37 and 90–97 (ink), 39, 40–47 and 100–107 (background), 49, `38;5;n` and `48;5;n` (the
  256-colour palette, drawn as its nearest of the first 16 in the 16-bit cells). Any other sequence is read to its
  end and does nothing.
- **Scrolling.** At the bottom the screen scrolls up and the row that leaves it goes into the scrollback ring in
  VRAM. Shift+PageUp and Shift+PageDown in the window move through it; the program does not see them.

The **error stream** (`ErrorOutput`, block command 3) is drawn the same way, in bright red; it has its own
colours and its own escape-sequence state, so what one stream leaves half-written the other does not finish.

An exception nobody handled is painted over the screen in white on red when the program stops, as well as going
to the log.

## Input

What is typed in the window goes through the terminal's **line discipline** before the program sees it.

**Cooked** (the default, `Mode` bit 0 clear): the line is edited in the terminal and handed over whole, with its
`\n`, when Enter is pressed. What is typed is echoed (with bit 1); Backspace and Delete remove a character, Left,
Right, Home and End move in the line, Ctrl+U empties it, and Up and Down walk the last 32 lines (with bit 2). Ctrl+D
on an empty line is the **end of the input**: `Status` bit 2 is set once the program has read everything before
it. On a line with text, Ctrl+D hands the line over without its newline. **Ctrl+C** drops the line, sets `Status`
bit 3 and raises interrupt 19; the standard library turns it into `SIGINT`. A line holds at most 4095 bytes.

**Raw** (`Mode` bit 0 set): each key goes to the program as it is pressed, with no echo, Ctrl+C and Ctrl+D
included. Keys without a character arrive as the bytes a terminal sends (SPEC 8.3): Esc `ESC`; the arrows
`ESC[A` (up), `ESC[B` (down), `ESC[C` (right), `ESC[D` (left); Home `ESC[H`; End `ESC[F`; Insert `ESC[2~`;
Delete `ESC[3~`; PageUp `ESC[5~`; PageDown `ESC[6~`. A character is its UTF-8, Enter `\n`, Backspace `\b`, and
Ctrl+A to Ctrl+Z the control characters 1 to 26. This is what `key.h` decodes.

The input holds 8192 bytes; with `Mode` bit 3, every arrival raises interrupt 19, so a program can `halt` until
there is something to read.

**Typed by a script.** `ceres run --type <file>` types a file on the terminal as if at the keyboard (a line end
is Enter; byte 3 is Ctrl+C and byte 4 Ctrl+D), and `--keys <file>` presses keys at instants of the machine's time.
Typed text is taken a key at a time **as the program reads** (`Status`, `Input`, `Available` or a block read), so it
goes through whatever mode the program is in by then: a menu that switches to raw after it starts still gets its
arrows as raw keys. A byte that is not UTF-8 reaches a raw program as it is. Without a window the input ends once
the script is done, so a program that reads to the end of its input finishes.

## Without a window

With `--headless` (or `CERES_HEADLESS`, or a build without SDL) the terminal still works; it only draws on a screen
nobody sees. The host keeps what the program wrote in files, if asked to:

- `--transcript <file>`: every byte the program wrote to its terminal, in order; the error stream's between
  `ESC[E` and `ESC[e`, so the two can be told apart and the file still reads as the screen did.
- `--screen-log <file>`: the text plane as plain text, a `--- present N ---` section at every `Present` and a
  `--- end ---` one when the program stops.

## The debug log (`0xFF030000`)

Messages for whoever runs the program, not for its user: they never go to the terminal. A line is written a
character at a time and sent when it ends; the host puts it in its **log** as `[ceres:<level>] <line>`, on stderr
or in the file of `ceres run --log <file>`. The host's own diagnostics about the run go to the same log (an
exception nobody handled is `[ceres:error] Unhandled …`). Under `ceres debug` the lines appear with the program's
error output.

| Offset | Register | Access | Meaning |
| --- | --- | --- | --- |
| `0x00` | `Output` | W | One character of the line; `\n` sends it. A line of 4096 characters is sent as it stands. |
| `0x04` | `Level` | RW | The level of the next line: `0` error, `1` warn, `2` info (the default), `3` debug. |
| `0x08` | `Flush` | W | Sends the unfinished line, if there is one. |
| `0x0C` | `Break` | W | Under `ceres debug`, stops on the next instruction; under `ceres run`, nothing. |
| `0x10` | `Enabled` | R | `1` when the host collects the log, so a program can skip building lines that go nowhere. |

The standard library's `ceres/debug.h` (`log_msg`, `LOGE` … `LOGD`, `dbg_hexdump`, `dbg_break`) writes here.

## Related pages

- [Video](31-Video.md) — the text plane the terminal draws on.
- [I/O devices and MMIO](07-IO-Devices-and-Ports.md) — the address map and the other devices.
- [CLI](16-CLI-and-Assembly-Pipeline.md) — `--headless`, `--transcript`, `--screen-log`, `--type`, `--keys`, `--log`.
- [Interrupts and exceptions](08-Interrupts-and-Exceptions.md) — interrupt 19.

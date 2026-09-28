# The shell and loading programs

A program can hand the machine over to another program, and `ceres run` can put a **shell** in front of everything:
start the machine, get a prompt, move through the host directory and run programs from it, as on a home computer.
The machine side is small - one command of the system control device - and the shell itself is an ordinary program
of the C library (Ceres STDLIB, `bin/shell/shell.c`).

## Loading and running from inside the machine: command 3

The [system control device](07-IO-Devices-and-Ports.md#systemcontroldevice-0xffff0000) has two write-only registers
for it, and a command:

| Offset | Register | Meaning |
| --- | --- | --- |
| `0x30` | `LoadPath` | The RAM address of a NUL-terminated path in the **host directory** (`--host-dir`, [HostFs](07-IO-Devices-and-Ports.md#hostfsdevice-0xff310000)): the `.cres` to run. |
| `0x34` | `LoadArgs` | The RAM address of three words - `argc`, the address of `argv` (`argc` string addresses) and the address of a NULL-terminated `envp` - or `0`. |
| `0x00` | `Command` | `3`: load the program at `LoadPath` and run it in place of this one. |

```casm
la   r1, path              // "games/snake.cres"
str  [r13 + 0x30], r1      // r13 = 0xFFFF0000
la   r1, block             // { 2, argv, 0 }: argv = { "games/snake.cres", "-fast" }, this environment
str  [r13 + 0x34], r1
li   r1, 3
str  [r13 + 0x00], r1      // the last instruction of this program - unless the load fails
```

What happens, in order:

1. The store reads the path and the block from RAM **at once**: the program may reuse that memory afterwards. A path
   or a string of up to 4095 bytes, and up to 256 arguments and as many environment entries, are read.
2. The host resolves the path the way HostFs does (relative, plain names, nothing outside the host directory), reads
   the `.cres` and checks it fits the machine with its arguments.
3. If all is well, the instruction after the store never runs: between the two the machine restarts **as on a
   reset** ([command 2](07-IO-Devices-and-Ports.md#systemcontroldevice-0xffff0000)), with the new image instead of
   the old one. The new program gets `argv` from the block (`argv[0]` is, by convention, its path) and the
   environment from `envp` - or the running program's environment when `envp` is `0`. With `LoadArgs` `0` it gets
   `argv = { path }` and the same environment. A later reset restarts the new program, not the old one.
4. If anything fails - no host directory, no such file, not a `.cres`, too big for the RAM, a string past the
   limits - nothing restarts: **the program goes on after its store**, as `execve` returns. The host's log says why
   (`[ceres:warn] Cannot load 'games/snake.cres': ...`), naming the file by the program's path.

**What survives the change.** Every device is reset, as on command 2 - the timer, DMA, audio, the GPU's modes and
registers, open HostFs files - except the terminal's session: its screen (the text plane's cells and scrollback, and
the cursor), and the line history. A program run from the shell writes below the shell's prompt, and the shell
comes back to its lines and its history. When the program has changed the text plane's geometry or cell format,
the screen starts clear; the history still comes back. The terminal's unread input stays too, as on any reset: what
was typed ahead for the shell is still there when it comes back - unless the program read it.

The C library wraps all of it: `sys_run(path, argc, argv, envp)` (`ceres/sys.h`), which flushes stdio first and
returns `-1` only when the load failed.

## `--shell`, and `ceres run` without a program

```bash
ceres run                                   # the shell, from <sysroot>/bin/shell.cres
ceres run --sysroot C:\ceres                # the same, with the sysroot named
ceres run game.cres --shell --sysroot C:\ceres   # game.cres first, then the shell
```

- **The shell** is `<sysroot>/bin/shell.cres`. The sysroot is `--sysroot <dir>`, or `CERES_SYSROOT` in the
  environment: the directory the STDLIB installs into (`make install PREFIX=<dir>`, or `tools\install.ps1 -Prefix
  <dir>`), the same one `ceresc --sysroot` takes. Without either, or with no shell there, `ceres run` says where it
  looked and exits with status 1.
- **`ceres run` without a program** runs the shell, and goes back to it whenever a program ends.
- **`--shell`** does the same after a program given by name: the program runs first, then the shell.
- **Going back.** When a program other than the shell ends - it shut the machine down, or an exception nobody
  handled stopped it (reported in the log and on the screen first) - the shell is loaded again, as by command 3: the
  terminal keeps its session, `argv` is `{ <the shell's path> }`, and the environment is **the environment of the
  program that ended**, with `CERES_STATUS=<its exit status>` in it. That is how the shell learns how the program
  ended, and how it remembers where it was: it gives the program `PWD=/<its directory>` and finds it there when it
  comes back.
- **The end.** When the shell itself ends (`exit`, or the end of its input), the run ends, with the shell's status.
- **The host directory.** With the shell, it is the current directory unless `--host-dir` names another.

## The shell

The STDLIB's shell (`bin/shell/shell.c`) prompts with its directory, `ceres:/games>`, and reads a line at a time on
the terminal - so the terminal's line editing and history (Up and Down) work, and carry over programs.

| Command | What it does |
| --- | --- |
| `help [command]` | The commands, or one of them |
| `ls [dir]` | The files (with their sizes) and directories (`name/`) in a directory, or one file |
| `cd [dir]` | Goes to a directory; `cd` alone, or `cd /`, to the top of the host directory. `..` and `.` work |
| `cat <file>...` | Shows what files hold |
| `run <file> [args]` | Runs a `.cres` with those arguments, then comes back; `snake` alone does the same for `snake` or `snake.cres` |
| `clear` | Clears the screen |
| `mem` | The RAM and how much of it is free, and the VRAM |
| `time` | The date and time (UTC) and how long the machine has been up |
| `info` | The machine: profile, CPU clock, memory, video level and resolution, terminal size, whether a host directory is attached |
| `reset` | Resets the machine (command 2): a clear screen, an empty history, the shell from the start |
| `exit [status]` | Ends the shell, and with it the run, with that status |

After a program that ended with a status other than 0 the shell says `(exit status N)`. Ctrl+C at the prompt drops
the line; Ctrl+D on an empty line (or the end of `--type`'s text) ends the shell. Errors go to the error stream, in
the error colour: `cd: no such directory: x`, `run: no such program: x`, `unknown command: x ('help' lists them)`.

Without a window the shell is driven like any program ([Running a program](16-CLI-and-Assembly-Pipeline.md#running-a-program)):

```bash
ceres run --sysroot C:\ceres --host-dir games --headless --type session.txt --transcript out.txt
```

## Related pages

- [I/O devices and ports](07-IO-Devices-and-Ports.md) - the system control device and HostFs.
- [The virtual terminal and the debug log](33-Terminal-and-Debug-Log.md) - the terminal the shell lives in.
- [CLI and assembly pipeline](16-CLI-and-Assembly-Pipeline.md) - every option of `ceres run`.

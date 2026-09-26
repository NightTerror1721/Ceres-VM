# 30 · The machine's clock

The machine keeps its own time. Nothing it does depends on how fast the host is: the CPU counts **cycles**,
every clock a program can read is worked out from them, devices act on **events** scheduled in cycles, and
the host only decides how long a run takes in real time - never what the machine sees. A program reads the
same instants on every run, which is what makes tests, recordings and the debugger's step back possible.
(The machine profiles - `micro` … `workstation` - are the second half of this page, written with plan/v2 F4.6.)

## Cycles

Every instruction costs a fixed number of cycles (plan/v2 SPEC 3.2, normative), counted in a 64-bit
counter the engine keeps (`ExecutionEngine::cycles()`):

| What | Cycles |
| --- | --- |
| ALU, `mov`, `li`, `lui`, logic, shifts, `cmp`, bit operations | 1 |
| A load or store that reaches RAM / VRAM / a device | 2 / 3 / 4 |
| A conditional jump, not taken / taken; `jp` | 1 / 2; 2 |
| `call`, `ret`, `bl`, `enter`, `leave` | 3 |
| `push`, `pop` | 2 |
| `pushm`, `popm`, `fpushm`, `fpopm` | 1 + 2 per register |
| `mul` and its family / `div`, `mod` and theirs | 3 / 16 |
| Float basics and conversions / `fdiv`, `fsqrt`, `fmod`, `frecipe`, `frsqrte` / `fma` | 3 / 12 / 4 |
| A block instruction (`mcpy`, `mset`, `mcmp`, `mscan`) | 4 + one per 8 bytes, rounded up |
| Entering an interrupt / `iret` | 12 / 6 |

The table lives in `libs/core/include/ceres/core/isa/cycles.h`: a base cost per opcode, plus what each
access, taken jump and block chunk adds where it happens. The costs model the simulated machine, not the
host - on the host a division costs what an addition does.

## Time

The **CPU clock** turns cycles into time: 50 MHz by default (`ceres run --cpu-clock`), kept by the
scheduler (`Scheduler::clockHz`) and reported to programs by the timer's `CpuClockHz` register and the
system control device's. At 50 MHz a cycle is 20 ns. From the cycle count the timer works out:

- the **nanoseconds** and **milliseconds** since the machine started;
- the **real-time clock**, seconds since 1970: where the machine's time started (the host's clock when the
  machine starts, or `ceres run --rtc YYYY-MM-DDThh:mm:ss`, UTC) plus the machine's time since.

Two reads with no instruction between them give the same instant. With `--rtc` every clock is the same on
every run; without it only the real-time clock's start differs. See [I/O devices](07-IO-Devices-and-Ports.md)
for the timer's registers.

## Events

A device that acts on its own - the timer's countdown and alarm, a DMA transfer landing - schedules an
**event** at an absolute cycle on the machine's **scheduler** (`libs/vm/include/ceres/vm/scheduler.h`) and
is called back (`IODevice::onEvent`) when the clock reaches it. There is no per-instruction call: the CPU
loop compares its cycle count with the next event's cycle once per instruction, and services what is due
at the start of the next step, before pending interrupts are looked at, so what an event raises is taken on
that step.

A **halted** machine executes nothing, and its clock jumps straight to the next event. With nothing
scheduled only the host can wake it (a key, input), and the halted step waits for that a few milliseconds
at a time.

## Pace

Because a halt jumps and a busy loop runs as fast as the interpreter can, machine time and host time only
meet where the host makes them. `ceres run --speed` says how:

| `--speed` | The host… |
| --- | --- |
| `realtime` | waits whenever the machine is ahead, so a second of machine time takes a second (the default while a window is open) |
| `max` | never waits (the default without a window) |
| `<f>x` (`0.5x`, `2x`…) | keeps `f` machine seconds to one of its own |

The driver runs the machine in **slices** of a millisecond of machine time, ending one early at a halt or a
frame of text presented for the window; between slices it pumps the window, presents the display (on a new
frame, or every 16 ms), injects the host's input and asks the **pacer** (`libs/driver/include/ceres/driver/pacer.h`)
whether to wait. The pacer sleeps at most 10 ms at a time so the window stays responsive, lets go of time
the host did not give (a slow host, a machine waiting for a key) rather than racing through it later, and
measures the effective speed for the window to show.

## Input

Everything the host gives the machine - standard input and its end, the console, a window's keys, text,
mouse and gamepad, files dropped on it - waits in the driver's **input hub** and goes in between two slices,
stamped with the cycle it went in at (`libs/driver/include/ceres/driver/input_journal.h`). Those stamps are
the whole of what the host decides about a run, so:

- `ceres run --record <file>` writes them, one line per event;
- `ceres run --replay <file>` feeds them back at the same cycles and reads no input of the host's.

A replay is the recorded run, whatever the host does now or however fast it is.

## Measuring

- `ceres profile` reports instructions and cycles by function and by source line
  ([CLI](16-CLI-and-Assembly-Pipeline.md#profiling-a-run)).
- The debugger shows the cycle count and the machine's time in `regs`, and takes `cycles` and `nanos` in
  expressions; its snapshots keep the cycle count and the scheduled events ([Debugger](22-Debugger.md)).
- The determinism check (`ctest -R determinism`) runs a program that waits on the timer at `--speed max`
  and `4x`, and requires the same output and the same final cycle count.

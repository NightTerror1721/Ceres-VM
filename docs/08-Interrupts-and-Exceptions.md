# Interrupts and exceptions

[← Back to index](README.md)

Ceres has a single unified mechanism for exceptions raised by the hardware (division by zero,
misaligned access, stack overflow, …), software-triggered interrupts (`int imm8`, `trap`), and
device-triggered interrupts (the timer). All of them go through `InterruptNumber`
(in [`interrupts.h`](../Ceres/libs/core/include/ceres/core/isa/interrupts.h)) and the same dispatch code in
`ExecutionEngine::triggerInterrupt()`.

## Interrupt number space

```cpp
enum class InterruptNumber : u8
{
    Reset              = 0,
    Trap               = 1,
    IllegalInstruction = 2,
    MemoryFault        = 3,
    DivisionByZero     = 4,
    StackOverflow      = 5,
    AlignmentFault     = 6,
    PageFault          = 7,
    // 8-14 currently unused
    Syscall            = 15,

    UserInterrupt0  = 16,
    // ... up to
    UserInterrupt47 = 63,
};
```

- **0–15 are reserved** (`ReservedInterruptCount = 16`) for system exceptions. These are **always
  deliverable**, regardless of the Interrupt flag (`sti`/`cli` only mask *user* interrupts).
- **16–63 are user interrupts** (48 of them), only delivered while the Interrupt flag is set. Four
  devices raise one today: the timer, always `UserInterrupt0` (16); the terminal, `UserInterrupt1`
  (17) whenever `pushInput()` adds a byte to its buffer; the DMA controller, `UserInterrupt2` (18)
  when a transfer completes; the keyboard and mouse, `UserInterrupt3` (19) and `UserInterrupt4`
  (20), when an event, a typed character or a motion arrives; the gamepad, `UserInterrupt5` (21); and
  the audio device, `UserInterrupt6` (22), when a tone has finished. The terminal also raises its
  interrupt when its input is closed — see
  [I/O devices and ports](07-IO-Devices-and-Ports.md#timerdevice-ports-0x10-0x12).

`MemoryFault` (3) is raised by a store whose target overlaps the loaded program's `.text` or lies in
the vector table or the BIOS (below `0x400`) — see
[Memory → `.text` is read-only](02-Memory.md#text-is-read-only) and
[Memory → the vector table and the BIOS are read-only](02-Memory.md#the-vector-table-and-the-bios-are-read-only). `PageFault` (7) is
raised by the MMU on a not-present or permission-violating translation, and only ever fires once a
program has turned paging on — see [Virtual memory and paging](27-Virtual-Memory-and-Paging.md). Of
the reserved numbers, `Syscall` (15) is defined but nothing raises it yet.

## The vector table

The first 256 bytes of memory (`0x00000000`–`0x000000FF`) are 64 entries of 4 bytes each: one 32-bit
handler address per interrupt number, indexed by `interruptNumber * 4`. Entry 0 doubles as the
**reset vector** — the address the program counter is initialized to on `reset()` — and, by
convention, the required entry point of every Ceres program (see
[Labels and symbols](12-Labels-and-Symbols.md), `main`).

The BIOS (see [`bios.h`](../Ceres/libs/vm/include/ceres/vm/bios.h)) initializes every vector from `Trap` through
`UserInterrupt0` to point at its own 3-instruction stub at `0x100` (which prints `'E'` and halts), so
an unhandled fault produces visible, if minimal, feedback instead of silently jumping through a null
pointer.

A running program can never overwrite this table itself — every store to an address below `0x400`
raises `MemoryFault` and does nothing (see [Memory](02-Memory.md#protected-vs-unrestricted-access)),
on purpose: a stray pointer must not be able to corrupt interrupt dispatch. A program that wants real handling of a
given interrupt instead declares `interrupt NUMBER: handler` (or `interrupt Name: handler`, using one
of the names above), and the *loader* patches that one vector before the program's first instruction
runs — see [Interrupt vector binding](26-Interrupt-Vector-Binding.md).

## Dispatch: what happens when an interrupt fires

`ExecutionEngine::triggerInterrupt(number)`:

1. If the Interrupt flag is clear **and** the interrupt number is a user interrupt (≥ 16), the
   interrupt is dropped — not queued, just discarded.
2. The handler address is read from the vector table (`memory[number * 4]`, unchecked read). If it's
   `0`, the interrupt is silently ignored (no handler installed).
3. The engine checks there's room on the stack for two 32-bit pushes. If not — the stack is already
   right at the edge of the protected region — it sets the Trap **and** Halting flags and stops,
   rather than risk pushing, overflowing, and re-entering the fault handler forever with nowhere left
   to record what happened.
4. It pushes the current flags (with the Halting bit masked out — see
   [Registers and flags](03-Registers-and-Flags.md#flags-register)), then the current program
   counter.
5. It clears both the Interrupt and Halting flags, then jumps to the handler address.

`iret` reverses steps 4–5 exactly: it pops the PC first (it's on top of the stack), then the flags,
and jumps to the restored PC — it does **not** advance the PC afterward, since the popped value
already points at the correct resumption point.

## Pending interrupts and `halt`

Before every instruction (including while halted), `ExecutionEngine::step()` checks for a pending
interrupt via `InterruptController::peek()`. A masked user interrupt is **not thrown away** — it
stays queued until the Interrupt flag is set. This is also the mechanism by which a device wakes a
halted machine: `triggerInterrupt()` clears the Halting flag as part of dispatch, so a `halt`ed CPU
resumes the instant a deliverable interrupt arrives.

## Faults the hardware raises on its own

These are triggered by `ExecutionEngine` itself, not by any explicit `int`/`trap` in your program:

| Interrupt | Raised when |
| --- | --- |
| `AlignmentFault` (6) | A 16- or 32-bit memory access (`ldr`/`ldrh`/`ldrsh`/`str`/`strh`/`fldr`/`fstr`) targets an address that isn't a multiple of its own size. Byte-sized accesses never trigger this. See [`checkAlignment<T>()`](../Ceres/libs/vm/include/ceres/vm/execution_engine.h). |
| `StackOverflow` (5) | A `push`/`call` would write below `0x400` (the protected segment), **or** a `pop`/`ret` would read past the top of memory (an unbalanced stack). See [Memory → The stack](02-Memory.md#the-stack). It's also raised (together with setting the Trap flag) if there isn't even room to push the two words an interrupt dispatch itself needs. |
| `IllegalInstruction` (2) | The fetched opcode byte doesn't map to any known instruction — the 256-entry dispatch table defaults every unused slot to an internal `INVALID` handler that raises this. |
| `PageFault` (7) | Paging is on (`pgon`) and a load, store or instruction fetch translates to a directory or table entry that isn't Present, or to one that is but doesn't grant the access (a write without the Writable bit, a fetch without Executable). `mfpf` reads back the address that faulted. See [Virtual memory and paging → Faults](27-Virtual-Memory-and-Paging.md#faults). |

Division and modulo by zero (`div`/`idiv`/`mod`/`imod`, and the float divisions) are, **by default**, a deliberate
exception to "faults go through the interrupt mechanism": they set the Trap flag directly and continue
execution with the destination register unchanged, without dispatching through the vector table at all.
A program that would rather be told writes `FeatureDivisionFault` to the features register of the
[system control device](07-IO-Devices-and-Ports.md#systemcontroldevice-0xffff0000): a division by zero
then raises `DivisionByZero` (4) instead, always deliverable like any reserved interrupt. It advances the
program counter first, like `trap`, so a handler that returns lands on the instruction after the division
(whose destination was left as it was) rather than repeating it forever; the Trap flag is not set.

**`sti` takes effect one instruction late.** The instruction right after an `sti` runs before any user
interrupt can be delivered, as on x86. That makes `sti; halt` a single indivisible step: an interrupt that
arrives between the two is delivered after the `halt` has run, and wakes it, rather than being serviced
first and leaving the machine to sleep with nothing left to wake it. See
[Instruction set → Arithmetic](05-Instruction-Set.md#arithmetic-0x10-0x28).

## Software-triggered interrupts

| Instruction | Effect |
| --- | --- |
| `int imm8` | Advances the PC, then raises interrupt `imm8`. Any of the 256 possible values is valid syntactically; only 0–63 map to a named `InterruptNumber`, but the dispatch code itself just indexes the 64-entry vector table by whatever number is given — values ≥ 64 read past the defined table into whatever memory follows it. |
| `trap` | Advances the PC, then raises `Trap` (1). Equivalent to `int 1`, spelled out as its own mnemonic for readability. |
| `reset` | Raises `Reset` (0) *without* first advancing the PC — since it's about to reinitialize the whole machine, the PC being about to change is moot. |

## Related pages

- [Memory](02-Memory.md) — the vector table's location, the protected segment, the stack.
- [Registers and flags](03-Registers-and-Flags.md) — the Interrupt/Halting/Trap flags.
- [I/O devices and ports](07-IO-Devices-and-Ports.md) — the timer, the terminal, the DMA controller, the keyboard and the mouse: the devices that raise a user interrupt today.
- [Instruction set → System control](05-Instruction-Set.md#system-control-0x00-0x07) — `int`, `iret`, `cli`, `sti`.
- [Interrupt vector binding](26-Interrupt-Vector-Binding.md) — how a program points a vector at its own handler.

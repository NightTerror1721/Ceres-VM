# Devices and the bus

[← Back to index](README.md)

How a device is built and how a program reaches it: the rules the bus enforces, how a fault says which rule it
broke, the table every device declares, where the files live, and what adding a device takes. The devices
themselves, register by register, are in [I/O devices and MMIO](07-IO-Devices-and-Ports.md). This is the
device model of the v2 machine ([plan/v2 SPEC §5](../plan/v2/SPEC.md)).

## Every register is 32 bits

A device lives in one 64 KiB slot of the window at `0xFF000000`–`0xFFFFFFFF` (slot `n` starts at
`0xFF000000 + n × 0x10000`). Its registers sit at word-aligned offsets inside the slot, and **each one is
32 bits wide and takes an aligned 32-bit access and nothing else**:

| Access | Result |
| --- | --- |
| `ldr`, `str`, `fldr`, `fstr` (and their indexed and PC-relative forms) at an offset aligned to 4 | One 32-bit access to the device |
| `ldrb`, `ldrh`, `ldrsb`, `ldrsh`, `strb`, `strh` | `MemoryFault`, reason `MmioWidth` |
| A misaligned access | `AlignmentFault`, reason `MmioWidth` |
| `mcpy`, `mset`, `mcmp`, `mscan` touching a device | `MemoryFault`, reason `MmioBlock` |
| A slot with no device | Reads `0xFFFFFFFF`; a write is dropped |
| An offset the device does not declare | Reads `0`; a write is dropped. With `ceres run --strict-mmio`: `MemoryFault`, reason `MmioUndeclared` |

A register that carries a byte — the terminal's output, for one — takes the word's **low byte**; the rest of the
word is ignored. A buffer goes through the device's block registers, not through a run of byte stores.

The checks cost a RAM access nothing: the width is known when the load or store is compiled into the
interpreter, and the undeclared-offset test is one bit of a mask the bus builds when the device is attached.

## Which rule a fault broke

A memory fault records where it happened and how (the system control device's `FaultAddressRegister` and
`FaultAccessRegister`) and, since v2, **why**: `FaultReasonRegister` (`0xFFFF002C`), one of these
([`fault_reason.h`](../Ceres/libs/vm/include/ceres/vm/fault_reason.h)):

| Code | Reason | When |
| --- | --- | --- |
| 0 | `None` | No reason recorded (a page fault, a store into `.text`) |
| 1 | `Alignment` | A misaligned access to RAM |
| 5 | `MmioWidth` | A device register reached by anything but an aligned 32-bit access |
| 6 | `MmioBlock` | A block instruction that touched a device |
| 7 | `MmioUndeclared` | An undeclared offset, under `--strict-mmio` |

Codes 2–4 and 8–10 are reserved for the phases that bring their causes (the memory map and the 64-bit
instructions). A device mapped by a page table somewhere else than its window still counts as a device: a
misaligned access through that page says `MmioWidth`.

## What a device is

```cpp
// libs/vm/include/ceres/vm/io_device.h
class IODevice {
public:
    virtual u32  read(Address offset) = 0;                 // an aligned offset inside the slot
    virtual void write(Address offset, u32 value) = 0;
    virtual const RegisterMap& registers() const = 0;       // the table below
    virtual void reset() {}                                 // a reset command: back to the power-on state
    // ... plus the timing hooks (needsTick, tick, ticksUntilEvent, advance) the clock uses today
protected:
    Memory& memory();
    void raiseInterrupt(InterruptNumber);
};
```

`read` and `write` stay a plain comparison or `switch` on the offset: the table is never consulted on the hot path.

### The register table

Every device declares all its registers in one table in its `.cpp`
([`register_map.h`](../Ceres/libs/vm/include/ceres/vm/register_map.h)):

```cpp
constexpr RegisterInfo Registers[] = {
    //  offset  name        access                     reset  read changes it  what it does
    { 0x00, "Status",   RegisterAccess::Read,      0x2, false, "Bit 0 input available, bit 1 ready for output, ..." },
    { 0x04, "Output",   RegisterAccess::Write,     0x0, false, "The low byte goes to the output stream." },
    { 0x08, "Input",    RegisterAccess::Read,      0x0, true,  "The next input byte, or 0 when there is none." },
    // ...
};

const RegisterMap& TerminalDevice::registers() const
{
    static constexpr RegisterMap map{ "terminal", Registers };
    return map;
}
```

- **access**: `Read`, `Write`, `ReadWrite` or `WriteOneToClear`.
- **reset**: what a read gives after a reset, when that does not depend on the machine or the host (0 otherwise).
- **read changes it**: reading pops a queue or latches a word. The debugger does not read such a register just to
  show it.

The table is what the bus uses for undeclared offsets, what `--strict-mmio` checks, what the debugger's `dev`
command shows, and what the device tests check: every table is non-empty, in offset order, aligned, below
`0x100` and described, and every device name is unique.

### Seeing it from the debugger

```text
(ceres) dev
  slot   0  0xFF000000  terminal           11 registers
  slot   1  0xFF010000  timer              11 registers
  ...
  slot 255  0xFFFF0000  system-control     10 registers
(ceres) dev timer
  timer at 0xFF010000
    0x00  Ticks              R   (not read)  The low word of the ticks so far; also latches the high word.
    0x04  Clock              R   0x68F5A0C1  Seconds since the epoch.
    0x08  Command             W  -           Arms the timer to fire after that many ticks; 0 disarms it.
    ...
```

## Where the files live

One device per file, with its header, its source and its test:

```text
Ceres/libs/devices/
  include/ceres/devices/
    devices.h                  every device, for a user that wants them all (declares none itself)
    audio/audio.h
    input/keyboard.h  mouse.h  gamepad.h
    storage/disk.h  host_fs.h  peripherals.h
    system/system_control.h  timer.h  dma.h
    terminal/terminal.h
    video/display.h  blitter.h  text_framebuffer.h  text_renderer.h  default_font.h
  src/<group>/<name>.cpp       the bodies, and the register table
  tests/test_<name>.cpp        one test file per device; device_test_machine.h holds what they share
```

`text_renderer.h` and `default_font.h` are helpers of the window, not devices. The `device_layout` check in ctest
([`check_device_layout.cmake`](../Ceres/tests/cli/check_device_layout.cmake)) fails when a header declares two
devices, when a device sits outside a group folder, or when one lacks its source or its test.

## Adding a device

1. **Header**: `include/ceres/devices/<group>/<name>.h`, one class deriving from `vm::IODevice`, its registers as
   `static inline constexpr Address XRegister = Address(0x..)` at word-aligned offsets below `0x100`, and
   `u32 read(Address) override`, `void write(Address, u32) override`,
   `const RegisterMap& registers() const override`. Only one-line members and what has to be `constexpr` stay in
   the header.
2. **Source**: `src/<group>/<name>.cpp` with the bodies and the `Registers` table; every offset `read` or `write`
   answers is in it.
3. **Slot**: its base in `default_mmio` ([`mmio_bus.h`](../Ceres/libs/vm/include/ceres/vm/mmio_bus.h)), and an
   `attachTo`/`detachFrom` pair that attaches it there.
4. **Hosts**: attach it where the machine is put together — `runMachine`
   ([`machine_runner.cpp`](../Ceres/libs/driver/src/machine_runner.cpp)) and the debugger's session
   ([`debug_session.cpp`](../Ceres/libs/debug/src/debug_session.cpp)).
5. **Test**: `tests/test_<name>.cpp`, including `device_test_machine.h`; add the device to the table test in
   `tests/e2e/test_devices.cpp` (`every_device_declares_a_sound_register_table`).
6. **Docs**: its section in [I/O devices and MMIO](07-IO-Devices-and-Ports.md), and its header in `devices.h`.

# F4 · Memoria, mapa nuevo y perfiles

- **Tamaño**: M · **Depende de**: F2 · **Repos**: CeresASM, STDLIB (Ceres-C sólo si usa direcciones de dispositivos que cambian)
- **Objetivo**: RAM hasta 2 GiB y VRAM hasta 1 GiB con reserva perezosa en el host; mapa MMIO por grupos e IRQ
  renumeradas; registro de depuración; los ocho perfiles.
- **SPEC**: §2, §4, §5.5, §5.6, §5.7 (DebugLog, SystemControl), §10.

## Punto de partida

- `Memory` (`Ceres/libs/vm/include/ceres/vm/memory.h`): `std::vector<u8>(size, 0)`, `MaxSize` 1 GiB, `DefaultSize` 16 MiB.
- `default_mmio` en `mmio_bus.h`: Terminal 0, Timer 1, Disk 2, Framebuffer 3, Dma 4, Keyboard 5, Mouse 6, Display 7,
  Gamepad 8, Audio 9, Peripherals 10, HostFs 11, Blitter 12, SystemControl 255.
- IRQ actuales: timer 16, terminal 17, DMA 18, 19–23 entrada, periféricos y audio, alarma 24, blitter 25.
- STDLIB: bases en `include/ceres.h` (`TERMINAL_BASE` … `SYS_CTRL_BASE`); números de IRQ en las cabeceras de cada dispositivo.
- Ceres-C: `libs/codegen/src/codegen.cpp` emite `la r4, 0xFFFF0000` (SystemControl, no cambia de sitio) y sus tests
  lo esperan; el terminal sigue en `0xFF000000`.

## Tareas

### F4.1 · Reserva perezosa de la RAM y 2 GiB

- **Repos**: CeresASM · **Depende de**: —
- **Archivos**: crear `Ceres/libs/core/include/ceres/core/base/host_pages.h` y `src/base/host_pages.cpp`
  (`VirtualAlloc`/`VirtualFree` en Windows; `mmap`/`munmap` en POSIX); `memory.h`.
- **Pasos**:
  1. `HostPages`: reserva y compromete `size` bytes con páginas a cero bajo demanda; movible, no copiable.
  2. `Memory` usa `HostPages` en lugar del vector; `MaxSize = 0x80000000`; el camino rápido sigue siendo un
     `memcpy` sobre memoria contigua.
  3. `sp` inicial = `RamSize` (comprueba que `0x80000000` no desborda en ningún cálculo con `u32`).
- **Aceptación**: [ ] Test: una máquina de 2 GiB arranca y ejecuta un programa. [ ] Puerta: con 2 GiB y un
  programa pequeño la memoria residente del proceso es de pocos MiB (apúntalo en «Notas»).
- **Commit**: `Reserve machine memory lazily and allow 2 GiB of RAM (F4.1)`

### F4.2 · VRAM y regiones del mapa físico

- **Repos**: CeresASM · **Depende de**: F4.1
- **Archivos**: crear `Ceres/libs/devices/include/ceres/devices/video/vram.h` y `.cpp` (o en `libs/vm` si el bus
  lo necesita antes que la GPU: decide según las dependencias de CMake y apúntalo); `execution_engine.h`;
  `memory_map.h` en `core/format`; `mmu.h`; debugger.
- **Pasos**:
  1. `Vram` con `HostPages`, tamaño `VramSize`, y un bitmap de páginas de 4 KiB escritas por la CPU (lo usará F12).
  2. Enrutado: `p < 0xA0000000` → RAM; si no, VRAM `[0xA0000000, 0xA0000000 + VramSize)`, MMIO `≥ 0xFF000000`, y
     el resto `MemoryFault` con `Unmapped`; fuera de lo respaldado, `OutOfRam`/`OutOfVram`.
  3. Costes de F2.1: 3 ciclos para VRAM.
  4. `blockSpan` devuelve un span directo cuando el bloque cae entero en la VRAM respaldada.
  5. La MMU acepta marcos de VRAM. DMA acepta VRAM como origen o destino.
  6. Debugger: `vram read <off> [n]`, `vram dump <off> <n> <archivo>`.
- **Aceptación**: [ ] Tests de cada región y de cada motivo de fallo. [ ] `benchmark_vm` sin pérdida > 1 %.
- **Commit**: `Add VRAM and the v2 physical memory map (F4.2)`

### F4.3 · Mapa MMIO por grupos e IRQ nuevas (tres repos)

- **Repos**: CeresASM, STDLIB, Ceres-C · **Depende de**: F4.2 · **SPEC**: §5.5, §5.6
- **Pasos**:
  1. **CeresASM**: `default_mmio` y los `Interrupt` de cada dispositivo según SPEC; tutoriales, ejemplos CASM,
     `bios.h` y tests e2e con las direcciones nuevas. Framebuffer, Display y Blitter se quedan temporalmente en
     slots libres del grupo de vídeo (`0x44`, `0x45`, `0x46`) hasta que F5 y F10 los retiren; apúntalo.
  2. **STDLIB**: bases de `include/ceres.h` y números de IRQ de cada cabecera; `asm/` si alguna rutina usa direcciones.
  3. **Ceres-C**: busca direcciones `0xFF0…` que cambien (el terminal y SystemControl no cambian); actualiza
     ejemplos y tests que usen otras.
- **Aceptación**: [ ] Las tres suites en verde. [ ] Ninguna dirección antigua (`0xFF02…`–`0xFF0C…` con el
  significado viejo) queda en los tres repos.
- **Commit** (uno por repo): `Regroup the device map and renumber the interrupts (F4.3)`

### F4.4 · Registro de depuración y `HostLog`

- **Repos**: CeresASM, STDLIB · **Depende de**: F4.3 · **SPEC**: §5.7
- **Pasos**:
  1. `system/debug_log.{h,cpp}` en el slot `0x03` con su tabla de registros y test.
  2. `HostLog` en `libs/driver`: escribe `[ceres:<nivel>] <línea>` a stderr o a `--log <archivo>`.
  3. Los diagnósticos de fallo que hoy imprime el host pasan por `HostLog`; el debugger atiende `Break`.
  4. STDLIB: `debug.h` (`log_msg`, `LOGx`, `dbg_hexdump`) escribe en el registro de depuración, no en `printf`;
     añade `dbg_break()`. Actualiza sus tests (la salida ya no está en stdout: compara el archivo de `--log`).
- **Commit** (uno por repo): `Add the debug log device (F4.4)`

### F4.5 · Perfiles de máquina · requiere P01

- **Repos**: CeresASM · **Depende de**: F4.4 · **SPEC**: §4
- **Archivos**: crear `Ceres/libs/driver/include/ceres/driver/profiles.h` y `src/profiles.cpp`; `apps/cli`.
- **Pasos**: tabla de perfiles; `--profile`; opciones sueltas que convierten el perfil en `custom`; comprueba
  límites (1920×1080 sólo en `custom`); `ProfileId` y `VramSize` en SystemControl; el perfil por defecto es
  `standard` (RAM de 64 MiB en lugar de los 16 MiB actuales).
- **Aceptación**: [ ] Test por perfil (valores publicados en los registros). [ ] `--max-resolution 1920x1080` con otro perfil lo convierte en `custom`.
- **Commit**: `Add machine profiles (F4.5)`

### F4.6 · Documentación

- **Repos**: CeresASM · **Depende de**: F4.5
- **Archivos**: `docs/02-Memory.md`, `07-IO-Devices-and-Ports.md`, `08-Interrupts-and-Exceptions.md`,
  `27-Virtual-Memory-and-Paging.md`, `30-Machine-Clock-and-Profiles.md` (añadir perfiles).
- **Commit**: `Document the v2 memory map, device map and profiles (F4.6)`

## Cierre de la fase

- [ ] Suites en verde en los tres repos. [ ] Puerta de memoria del host. [ ] `benchmark_vm` dentro del 1 %. [ ] Revisión `ocr`.

## Notas

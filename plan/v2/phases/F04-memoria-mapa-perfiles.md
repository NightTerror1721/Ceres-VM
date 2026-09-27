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
- **Aceptación**: [x] Test: una máquina de 2 GiB arranca y ejecuta un programa. [x] Puerta: con 2 GiB y un
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
- **Aceptación**: [x] Tests de cada región y de cada motivo de fallo. [x] `benchmark_vm` sin pérdida > 1 % (en `mixed`;
  `block-256`, −4 %: ver «Notas»).
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
- **Aceptación**: [x] Las tres suites en verde. [x] Ninguna dirección antigua (`0xFF02…`–`0xFF0C…` con el
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
- **Aceptación**: [x] Test por perfil (valores publicados en los registros). [x] `--max-resolution 1920x1080` con otro perfil lo convierte en `custom`.
- **Commit**: `Add machine profiles (F4.5)`

### F4.6 · Documentación

- **Repos**: CeresASM · **Depende de**: F4.5
- **Archivos**: `docs/02-Memory.md`, `07-IO-Devices-and-Ports.md`, `08-Interrupts-and-Exceptions.md`,
  `27-Virtual-Memory-and-Paging.md`, `30-Machine-Clock-and-Profiles.md` (añadir perfiles).
- **Commit**: `Document the v2 memory map, device map and profiles (F4.6)`

## Cierre de la fase

- [x] Suites en verde en los tres repos. [x] Puerta de memoria del host. [x] `benchmark_vm` dentro del 1 %. [x] Revisión `ocr`.

## Notas

- **F4.1**: `HostPages` (`core/base/host_pages.h`) reserva con `VirtualAlloc(MEM_RESERVE | MEM_COMMIT)` en Windows
  (comprometer sólo cuenta contra el límite de compromiso; la página se pone a cero y se hace residente al tocarla) y
  con `mmap(MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE)` en POSIX; es movible y no copiable. `Memory` la usa en lugar
  del `std::vector`, con `MaxSize = 0x80000000`; el camino rápido sigue siendo un `memcpy` sobre memoria contigua. El
  `sp` del programa arranca en `RamSize - 4 KiB` y el de los manejadores en `RamSize` (`0x80000000` con 2 GiB, que
  cabe en `u32`; los cálculos de pila ya iban en `u64` o no pasan de `RamSize`). Test `libs/vm/tests/test_memory.cpp`:
  una máquina de 2 GiB guarda en `0x40000000`, apila, desapila y atiende una interrupción en la pila del sistema.
  **Puerta de memoria** (`ceres run 01_hola.casm --terminal`, pico del conjunto de trabajo): con `--memory 1073741824`
  el binario anterior llega a 1033 MiB y el nuevo a 6,0 MiB; con 2 GiB, 6,0 MiB (con 16 MiB, 5,0). `benchmark_vm`
  (gcc-ipo, antes y después intercalados, tres pasadas estables): `mixed` 135,4 y 136,1 MIPS, `ram` 111,8 y 111,4.
  Pendiente para quien lo necesite: el historial del debugger (`history.cpp`) copia y compara la RAM entera en cada
  instantánea, así que con mucha RAM toca todas sus páginas; con el perfil `standard` (64 MiB) es asumible.
- **F4.2**: `Vram` está en `libs/vm/include/ceres/vm/vram.h`, no en `libs/devices`: el motor la necesita en cada acceso
  fuera de la RAM y `devices` depende de `vm`, no al revés. `CeresVM(ramSize, vramSize)` la crea (32 MiB por defecto,
  el de `standard`, hasta que F4.5 traiga los perfiles), el motor y el bus la reciben, y cada `IODevice` la alcanza con
  `vram()`. Tamaño de 16 KiB a 1 GiB en múltiplos de 4 KiB; la RAM pasa a exigir también múltiplos de 4 KiB (SPEC 2),
  para que un trozo de una instrucción de bloque, que nunca cruza una página, caiga entero dentro o fuera. El mapa de
  páginas escritas (`written`, `writtenPages`, `clearWritten`) lo marcan los almacenamientos de la CPU, los bloques y la
  DMA. Las constantes del mapa (`RamLimitValue`, `VramStartValue`, `VramLimitValue`, `MmioStartValue`) están en
  `core/format/memory_map.h`. **Enrutado** (`read`/`write` del motor): `p < 0xA0000000` compara con el final de la RAM
  (`Memory::readBacked`/`writeBacked`: la página nula y la BIOS siguen leyendo 0 e ignorando escrituras); lo demás va a
  `readOutsideRam`/`writeOutsideRam`: VRAM (3 ciclos), MMIO, o `MemoryFault` con `OutOfRam` (por debajo de
  `0x80000000`), `OutOfVram` (`0xA0000000–0xDFFFFFFF`) o `Unmapped`. Antes, fuera de la RAM se leía 0 en silencio.
  **Ejecutar**: desde la RAM y la VRAM; en otro sitio, `MemoryFault` con acceso `Execute` y el mismo motivo, y
  `MmioWidth` en la ventana de dispositivos (decisión de esta tarea: la SPEC no dice nada de ejecutar fuera de la RAM).
  Un `ldrd` sobre VRAM es un acceso (1 + 3 ciclos). Los bloques usan un span directo en RAM o VRAM; si no, van byte a
  byte y paran en el primer fallo sin mover los registros. La MMU no comprueba marcos: uno en la VRAM funciona sin más
  (las tablas de páginas siguen en la RAM). La DMA acepta RAM o VRAM en cada lado y recorta en el final de cada una; una
  dirección en ninguna no mueve nada. Debugger: `readMemory` lee también la VRAM por su dirección física (así `x
  0xA0000000` funciona), `vram read <off> [n]` y `vram dump <off> <n> <archivo>`. Tests: `test_memory.cpp` (cada región
  y cada motivo, ciclos, páginas escritas, bloques, ejecución), `test_dma.cpp`, `test_paging.cpp` (un marco de VRAM) y
  `test_debugger.cpp`. `benchmark_vm` (gcc-ipo, intercalado con F4.1): `mixed` 136,1 → 140,4 (+3 %), `ram` +3 %,
  `push-pop` +3 %, `block-256` 125,5 → 120,2 (−4 %). La primera versión perdía un 9 % en bloques; sacar de línea el
  camino de VRAM y los bucles byte a byte lo dejó en −4 %, y el resto parece disposición del código, como en F3.2.
- **F4.3**: `default_mmio` por grupos (SPEC 5.5): terminal `0x00`, timer `0x01`, DMA `0x02`, teclado `0x10`, ratón
  `0x11`, gamepad `0x12`, audio `0x20` (un slot: los cuatro de la SPEC llegan con F9/F11), disco `0x30`, HostFs `0x31`,
  periféricos `0x32`, y **provisionalmente** framebuffer de texto `0x44`, display `0x45` y blitter `0x46` hasta que F5 y
  F10 los retiren. IRQ (SPEC 5.6): cuenta atrás 16, alarma 17, DMA 18, terminal 19, teclado 20, ratón 21, gamepad 22,
  periféricos 26, audio 28 y el blitter 34 (la del «motor de copia» de la GPU, en la que se convertirá). El disco y
  HostFs no lanzan interrupciones todavía: sus 24 y 25 quedan reservadas y la STDLIB no les da nombre. `bios.h` no
  cambia (su último vector por defecto sigue siendo el 16). Las direcciones antiguas se reescribieron con un mapa de
  una sola pasada (la DMA ocupa el antiguo slot del disco): ejemplos (`pong.casm`, tutoriales 08 y 09), tests de driver,
  debug y e2e, y los documentos de CeresASM (07, 08, 16, 26 y 29; la tabla de slots de 07 lleva ya la IRQ de cada uno,
  y 08 la tabla de IRQ; el resto de 07 lo completa F4.6). `docs/README.md` y `docs/29` conservan los números antiguos
  donde cuentan historia. **Ceres-C**: sólo el test de `--run-arg` leía un registro de periféricos (`0xFF32002C`); los
  tests de `__interrupt_vector` y `docs/10-Interrupts.md` pasan el terminal al 19 y la tabla de IRQ a la nueva.
  **STDLIB**: bases de `ceres.h`, `interrupts.h` (`IRQ_ALARM` 17, `IRQ_TERMINAL` 19 … `IRQ_BLITTER` 34,
  `IRQ_DEVICE_LAST` 34), el módulo `irq` enlaza un stub sólo para los números que lanza un dispositivo (hay huecos:
  `irq_attach` devuelve −1 en los demás) y `irq_name` los nombra; comentarios de las cabeceras; `test_user_irq17`
  pasa a `test_user_irq19`; referencia regenerada con `node tools/gendocs.js`.
- **F4.4**: `DebugLogDevice` (`devices/system/debug_log.{h,cpp}`, slot `0x03`, `default_mmio::DebugLog`) con los
  registros de SPEC 5.7; `Level` guarda 0–3 (un valor mayor cuenta como 3), una línea sin fin se envía a trozos de
  4096 bytes y un reset tira la línea a medias y vuelve a `info`. `Enabled` es 1 cuando el host ha puesto un sumidero.
  `HostLog` (`driver/host_log.{h,cpp}`) escribe `[ceres:<nivel>] <línea>` (`error`, `warn`, `info`, `debug`) en el
  flujo de diagnósticos o en el archivo de `--log` (sólo `ceres run`; un archivo que no se puede escribir es un
  error antes de arrancar), con un mutex porque escriben la máquina y el host. Pasan por él el registro de
  depuración y **todos** los diagnósticos de `runMachine` (disco, `--host-dir`, puertos, carga, grabación,
  reproducción y la excepción sin manejar, que ahora sale como `[ceres:error] Unhandled …`); el informe de
  `ceres profile` sigue siendo texto suelto en stderr, y el flujo de error del terminal sigue yendo al stderr del
  host hasta F5. `driver::Machine` (la API para incrustar) lo expone como `MachineHost::debugLog`. **Debugger**: las
  líneas van a la salida de error de la sesión (o a la normal si no hay) con el mismo formato, y `Break` detiene el
  programa en la instrucción siguiente con `StopReason::Breakpoint` y el mensaje «The program asked to stop: …»;
  ninguna de las dos cosas actúa mientras se reproduce el historial. **STDLIB**: `DEBUGLOG_BASE` en `ceres.h`;
  `log_msg`/`LOGx`, `dbg_hexdump`, `dbg_where` y `dbg_span_end` escriben en el registro (el formato con
  `__vformat_ext`, carácter a carácter) y ya no llevan su `[nivel]`, que pone el host; nuevos `dbg_break()` y
  `dbg_log_enabled()`. **Desviación**: los tests comparan el log en stderr (`tests/expected/test_debug_ansi.stderr`,
  que el runner ya comprobaba) en lugar de un archivo de `--log`: es el mismo texto y no hace falta tocar el runner;
  el paso a `--log` se prueba en CeresASM (`driver_run.log_sends_the_hosts_log_to_a_file`).
- **F4.5**: `driver/profiles.{h,cpp}`: `ProfileId` (0 micro … 7 custom), `MachineProfile` (relojes, RAM, VRAM,
  vídeo máximo, resolución máxima, audio máximo, sprites por línea) con la tabla de SPEC 4, y `checkCustomProfile` con
  los techos de `custom` (CPU ≤ 400 MHz, GPU ≤ 1 GHz, RAM de 8 KiB a 2 GiB y VRAM de 16 KiB a 1 GiB en múltiplos de
  4 KiB, V0–V6, A0–A4, de 8×16 a 1920×1080). `custom` a secas son los relojes y la memoria de `standard` con los
  techos de `custom` en lo demás. El analizador junta `--profile` y las opciones sueltas (`--cpu-clock`,
  `--gpu-clock`, `--ram`, `--vram`, `--max-video`, `--max-audio`, `--max-resolution`) en un `MachineProfile` que
  sustituye a `memorySize`/`cpuClockHz` en `run`, `profile` y `debug` (las tres ejecutan una máquina; las demás
  órdenes rechazan estas opciones). **`--memory` desaparece**: `--ram` ocupa su sitio (SPEC 10) y admite `K`, `M`,
  `G` (de 1024); los relojes, `k`, `M`, `G` (de 1000). `Memory::DefaultSize` pasa a 64 MiB (un `static_assert` lo ata
  a `standard`, igual que `Vram::DefaultSize`). SystemControl publica `ProfileId` (`0x28`) y `VramSize` (`0x38`); el
  runner, `driver::Machine` (`MachineConfig::machine`) y el debugger (`LaunchConfig::vramSize`, `cpuClockHz`,
  `profileId`) montan la máquina del perfil. De momento sólo los relojes de CPU, la RAM, la VRAM y el identificador
  hacen algo: GPU, niveles, resolución y sprites se guardan para F5 y siguientes. Tests: `driver_command`
  (cada perfil, `--max-resolution` desde `micro` → `custom` con el resto de `micro`, límites, órdenes que no los
  admiten) y `driver_run.the_machine_publishes_its_profile` (un programa lee MemorySize, CpuClockHz, ProfileId y
  VramSize en los ocho perfiles). `tests/cli/cli_contract.cmake` usa `--ram`. La extensión de VS Code pasa `--ram`
  (y `profile`, nuevo en `launch.json`) al debugger; falta empaquetarla. **STDLIB**: `CERES_DEFAULT_RAM` a 64 MiB,
  `SYS_CTRL_CPU_CLOCK`/`PROFILE`/`FAULT_REASON`/`VRAM_SIZE`, `SYS_PROFILE_*`, `sys_profile()` y `sys_vram_size()`, y
  `test_sys` comprueba la máquina por defecto. **Coste pendiente**: con 64 MiB, el historial del debugger compara 64 MiB
  por instantánea (cada 20 000 instrucciones por defecto); la suite de debug pasó de 0,7 s a 2 s. Si molesta, la
  salida es que `HostPages` pida al sistema qué páginas se escribieron (`MEM_WRITE_WATCH`/`GetWriteWatch` en Windows).
- **F4.6**: `02-Memory` (el mapa físico, tamaños por perfil, reserva perezosa, VRAM, fallos fuera de rango en lugar
  de ceros, ejecución fuera de la RAM), `07-IO-Devices-and-Ports` (el mapa de direcciones, `ProfileId`,
  `VramSize`, los motivos 2–4, la DMA con VRAM y una sección nueva del registro de depuración), `08` (la tabla de
  IRQ, ya en F4.3), `27-Virtual-Memory-and-Paging` (marcos de VRAM y tamaños), `30-Machine-Clock-and-Profiles`
  (sección «Profiles» con la tabla, las opciones sueltas y los registros), y lo que quedaba de `--memory` en 09,
  12, 16, 19, 22 y 24, además de `35-Devices-and-Bus` (motivos 2–4), `22-Debugger` (`vram read`/`vram dump`, el
  log y `Break`) y el índice.
- **Cierre**: las tres suites pasan sobre CeresASM a5d6b29 (CeresASM 9/9, Ceres-C 11/11, STDLIB 311 comprobaciones).
  Puerta de memoria: la de F4.1 (6 MiB residentes con 2 GiB de RAM). `benchmark_vm` (gcc-ipo, el binario de antes
  de F4 y el final intercalados, cuatro pasadas, con el PC en uso): `mixed` 124,7 → 125,4 MIPS (+0,5 %); `ram` +6 %,
  `mmio` +10 %, `call-ret` +6 %, `push-pop` +3 %; `loop` −2,7 % y `block-256` −1,2 %. Revisiones `ocr` en modo
  delegado (el normal sigue sin responder): F4.1–F4.3 → 896d43c (un `--memory` inválido terminaba con `terminate`;
  el historial del debugger no guardaba la VRAM); F4.4–F4.6 → a5d6b29 (una máquina que el host no puede reservar
  terminaba con `bad_alloc`; la CLI anunciaba el `Break` como «Breakpoint 0»). Aparte, 1bc5ce5: dos tests del
  debugger de F4.2 y F4.4 acababan en `ret` sin apagar la máquina y la suite de debug tardaba 48 s en lugar de 1.

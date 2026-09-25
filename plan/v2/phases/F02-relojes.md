# F2 · Relojes y planificador de eventos

- **Tamaño**: L · **Depende de**: F1, F0.6 (P02) · **Repos**: CeresASM, STDLIB (y Ceres-C si algún test depende de ticks)
- **Objetivo**: la CPU cuenta ciclos según la tabla de SPEC §3.2; el tiempo de la máquina es virtual y
  determinista; un planificador de eventos sustituye al tick por instrucción; el ritmo respecto al host se elige
  con `--speed`.
- **SPEC**: §3, §5.7 (Timer, SystemControl `CpuClockHz`), §10.

## Punto de partida

- Un tick por instrucción; las instrucciones de bloque cuestan `1 + bytes/16`.
- `IODevice::needsTick`, `tick`, `ticksUntilEvent`, `advance`; `MmioBus::tick`, `ticksUntilNextEvent`, `rebuildTickedDevices`.
- `ExecutionEngine::setHaltClock`, `_haltClockHz`, `_haltCarryNanos`, `haltedStep`; `DefaultHaltClockHz` (100 MHz) en `mmio_bus.h`.
- `TimerDevice`: nanos y milis del `steady_clock` del host; alarma consultada cada `AlarmPollTicks` (1024)
  instrucciones; `HaltClockRegister` (`0x1C`). En Windows, la espera de un halt usa un temporizador de alta resolución.
- Test `tests/e2e/test_halt_clock.cpp`.
- Runner con ventana: `libs/driver/src/machine_runner.cpp` ejecuta `instructionsPerFrame()` (4096) y llama a `pump`/`present`.
- STDLIB: `include/ceres/timer.h` (ticks = instrucciones, `timer_halt_clock`, `timer_nanos64`), `src/time.c`,
  `include/ceres/game.h` (ritmo por ticks o por reloj real).

## Tareas

### F2.1 · Tabla de ciclos y contador

- **Repos**: CeresASM · **Depende de**: F1.10
- **Archivos**: crear `Ceres/libs/core/include/ceres/core/isa/cycles.h` (tabla `constexpr` indexada por opcode);
  `execution_engine.h` (contador `u64 _cycles`, `cycles()`); coste de bloque `4 + ceil(bytes/8)`.
- **Pasos**: suma el coste en el bucle de ejecución y en las entradas a interrupción; distingue RAM, VRAM
  (aún no existe: deja el hueco) y MMIO en cargas y almacenamientos; salto tomado o no. El timer sigue igual.
- **Aceptación**: [ ] Test que ejecuta secuencias conocidas y comprueba el total de ciclos. [ ] `benchmark_vm` sin pérdida > 1 %.
- **Commit**: `Count CPU cycles per instruction (F2.1)`

### F2.2 · Planificador de eventos

- **Repos**: CeresASM · **Depende de**: F2.1
- **Archivos**: crear `Ceres/libs/vm/include/ceres/vm/scheduler.h` y `src/scheduler.cpp`; `io_device.h`
  (`onEvent(u64)`, acceso a `scheduler()`); `execution_engine.h`; `system/timer.{h,cpp}`.
- **Pasos**:
  1. `Scheduler`: `schedule(IODevice&, u64 cycle, u32 tag)`, `cancel`, `nextCycle()`, `service(u64 now)`.
     Estructura: montículo pequeño o array ordenado (pocos dispositivos).
  2. En el bucle: `if (_cycles >= _scheduler.nextCycle()) _scheduler.service(_cycles);`.
  3. Migra la cuenta atrás y la alarma del Timer al planificador.
- **Aceptación**: [ ] Tests del Timer con el planificador. [ ] Sin pérdida de rendimiento.
- **Commit**: `Add the event scheduler and move the timer onto it (F2.2)`

### F2.3 · Todos los dispositivos al planificador; halt por eventos

- **Repos**: CeresASM · **Depende de**: F2.2
- **Pasos**:
  1. Migra DMA, entrada, audio y periféricos a `onEvent`.
  2. Elimina `needsTick`, `tick`, `ticksUntilEvent`, `advance`, `MmioBus::tick`, `ticksUntilNextEvent`,
     `rebuildTickedDevices`.
  3. `halt` adelanta `_cycles` al siguiente evento; sin eventos, la VM devuelve el control al runner (que espera
     entrada del host). Elimina `setHaltClock`, `_haltClockHz`, `_haltCarryNanos`, `DefaultHaltClockHz`.
  4. Sustituye `test_halt_clock.cpp` por `test_scheduler.cpp` con los mismos escenarios en ciclos.
- **Aceptación**: [ ] Ningún símbolo de tick por instrucción queda. [ ] Suites en verde.
- **Commit**: `Drive every device from the scheduler and let halt jump to the next event (F2.3)`

### F2.4 · Timer v2 y `CpuClockHz`

- **Repos**: CeresASM · **Depende de**: F2.3 · **SPEC**: §5.7
- **Pasos**: aplica la tabla de registros del Timer de SPEC §5.7; nanos y milis derivados de `_cycles` y
  `CpuClockHz`; RTC = valor de arranque + tiempo virtual; opción `--rtc`; `CpuClockHz` en SystemControl (`0x24`).
- **Aceptación**: [ ] Dos ejecuciones dan los mismos nanos. [ ] `--rtc` fija el RTC.
- **Commit**: `Redesign the timer on virtual time (F2.4)`

### F2.5 · Runner por tiempo virtual, `Pacer` y `--speed`

- **Repos**: CeresASM · **Depende de**: F2.4
- **Archivos**: `libs/driver/src/machine_runner.cpp`; crear `libs/driver/include/ceres/driver/pacer.h` y `src/pacer.cpp`;
  `apps/cli` (opciones `--speed`, `--cpu-clock`).
- **Pasos**: el bucle ejecuta hasta el siguiente milisegundo virtual (o el siguiente evento de presentación),
  bombea la entrada y acompasa según `--speed` (`realtime`, `max`, `<f>x`). Calcula la velocidad efectiva y
  exponla al backend (se mostrará en la barra de estado en F5).
- **Aceptación**: [ ] Con `--speed realtime` un programa que espera 2 s virtuales tarda unos 2 s reales. [ ] Con `max`, lo que tarde el host.
- **Commit**: `Pace the machine on virtual time (F2.5)`

### F2.6 · Entrada sellada, `--record` y `--replay`

- **Repos**: CeresASM · **Depende de**: F2.5
- **Pasos**: todo evento del host (teclas, texto, ratón, gamepad, archivo soltado) se inyecta en un punto de
  inyección y se sella con su ciclo; `--record` escribe la secuencia; `--replay` la inyecta en los mismos ciclos
  e ignora la entrada real.
- **Aceptación**: [ ] Test: grabar una sesión con entrada guionizada y reproducirla da el mismo resultado.
- **Commit**: `Stamp host input with its cycle and add record and replay (F2.6)`

### F2.7 · Perfilador y debugger en ciclos

- **Repos**: CeresASM · **Depende de**: F2.3
- **Pasos**: `ceres profile` informa ciclos por función (además de instrucciones); el debugger muestra `_cycles`
  y el tiempo virtual; los snapshots guardan `_cycles` y el estado del planificador.
- **Commit**: `Report cycles in the profiler and the debugger (F2.7)`

### F2.8 · STDLIB en tiempo virtual

- **Repos**: STDLIB · **Depende de**: F2.4
- **Archivos**: `include/ceres/timer.h`, `src/ceres/timer.c` (o donde esté), `src/time.c`, `include/ceres/game.h`
  y su fuente, tests que usen ticks o `timer_halt_clock`.
- **Pasos**: nuevos offsets del Timer; `timer_cycles64()`, `timer_nanos64()`, `timer_cpu_hz()`; elimina
  `timer_halt_clock` y lo deprecado (`ns64` sigue hasta F6); `clock()` en microsegundos virtuales; esperas sobre
  la alarma; `game.h` acompasa por tiempo virtual. Revisa a mano los `.expected` que cambien.
- **Aceptación**: [ ] `runtests.ps1` en verde. [ ] La documentación de `timer.h` explica ciclos y tiempo virtual.
- **Commit**: `Move the library onto the virtual clock (F2.8)`

### F2.9 · Test de determinismo y documentación

- **Repos**: CeresASM · **Depende de**: F2.6, F2.8
- **Archivos**: test e2e nuevo; crear `docs/30-Machine-Clock-and-Profiles.md` (la parte de relojes; los perfiles en F4.6).
- **Pasos**: el test ejecuta el mismo programa con `--speed max` y con `--speed 4x` y compara salida y ciclos finales.
- **Commit**: `Test determinism across speeds and document the machine clock (F2.9)`

## Cierre de la fase

- [ ] Suites en verde. [ ] `benchmark_vm` igual o mejor que en `BASELINE.md`. [ ] Revisión `ocr`.

## Notas

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
- **Aceptación**: [x] Test que ejecuta secuencias conocidas y comprueba el total de ciclos. [x] `benchmark_vm` sin pérdida > 1 %.
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
- **Aceptación**: [x] Tests del Timer con el planificador. [x] Sin pérdida de rendimiento.
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
- **Aceptación**: [x] Ningún símbolo de tick por instrucción queda. [x] Suites en verde.
- **Commit**: `Drive every device from the scheduler and let halt jump to the next event (F2.3)`

### F2.4 · Timer v2 y `CpuClockHz`

- **Repos**: CeresASM · **Depende de**: F2.3 · **SPEC**: §5.7
- **Pasos**: aplica la tabla de registros del Timer de SPEC §5.7; nanos y milis derivados de `_cycles` y
  `CpuClockHz`; RTC = valor de arranque + tiempo virtual; opción `--rtc`; `CpuClockHz` en SystemControl (`0x24`).
- **Aceptación**: [x] Dos ejecuciones dan los mismos nanos. [x] `--rtc` fija el RTC.
- **Commit**: `Redesign the timer on virtual time (F2.4)`

### F2.5 · Runner por tiempo virtual, `Pacer` y `--speed`

- **Repos**: CeresASM · **Depende de**: F2.4
- **Archivos**: `libs/driver/src/machine_runner.cpp`; crear `libs/driver/include/ceres/driver/pacer.h` y `src/pacer.cpp`;
  `apps/cli` (opciones `--speed`, `--cpu-clock`).
- **Pasos**: el bucle ejecuta hasta el siguiente milisegundo virtual (o el siguiente evento de presentación),
  bombea la entrada y acompasa según `--speed` (`realtime`, `max`, `<f>x`). Calcula la velocidad efectiva y
  exponla al backend (se mostrará en la barra de estado en F5).
- **Aceptación**: [x] Con `--speed realtime` un programa que espera 2 s virtuales tarda unos 2 s reales. [x] Con `max`, lo que tarde el host.
- **Commit**: `Pace the machine on virtual time (F2.5)`

### F2.6 · Entrada sellada, `--record` y `--replay`

- **Repos**: CeresASM · **Depende de**: F2.5
- **Pasos**: todo evento del host (teclas, texto, ratón, gamepad, archivo soltado) se inyecta en un punto de
  inyección y se sella con su ciclo; `--record` escribe la secuencia; `--replay` la inyecta en los mismos ciclos
  e ignora la entrada real.
- **Aceptación**: [x] Test: grabar una sesión con entrada guionizada y reproducirla da el mismo resultado.
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
- **Aceptación**: [x] `runtests.ps1` en verde. [x] La documentación de `timer.h` explica ciclos y tiempo virtual.
- **Commit**: `Move the library onto the virtual clock (F2.8)`

### F2.9 · Test de determinismo y documentación

- **Repos**: CeresASM · **Depende de**: F2.6, F2.8
- **Archivos**: test e2e nuevo; crear `docs/30-Machine-Clock-and-Profiles.md` (la parte de relojes; los perfiles en F4.6).
- **Pasos**: el test ejecuta el mismo programa con `--speed max` y con `--speed 4x` y compara salida y ciclos finales.
- **Commit**: `Test determinism across speeds and document the machine clock (F2.9)`

## Cierre de la fase

- [ ] Suites en verde. [ ] `benchmark_vm` igual o mejor que en `BASELINE.md`. [ ] Revisión `ocr`.

## Notas

- **F2.1**: la tabla (`cycles.h`) guarda la parte fija de cada opcode; cada carga o almacenamiento suma lo que
  alcanzó (RAM 2, dispositivo 4; VRAM 3 queda definido para F4), el salto tomado suma 1, las instrucciones de bloque
  cobran `ceil(n/8)` por trozo y 4 al terminar, y la entrada a interrupción e `iret` fijan su total (12 y 6). Así
  `push`, `call`, `pushm`, `enter` salen de la tabla de la SPEC sin casos especiales. `benchmark_vm` frente a BASELINE:
  igual o mejor en todo (ram 108,3 frente a 108,4); frente a F1.10 los saltos bajan de 181 a 170 MIPS por el ciclo
  del salto tomado.
- **F2.2**: el `Scheduler` vive en el `MmioBus` (donde se conectan los dispositivos, que lo alcanzan con
  `scheduler()`) y el motor lo ata a su `_cycles` y a un `_nextEvent` propio (`bindTo`), así que la comprobación
  por instrucción compara dos palabras vecinas. Un evento por pareja (dispositivo, etiqueta); `onEvent(tag, cycle)`
  lleva también la etiqueta, que el plan no nombraba. Se atiende **al principio** de `step()`, antes de mirar las
  interrupciones pendientes: lo que levante se entrega en ese mismo paso, como con el tick al final del anterior;
  sólo cambia lo que se ve entre dos pasos. El Timer cuenta **ciclos**: `TicksRegister` lee `_cycles`, la cuenta
  atrás es un evento y el periódico se rearma desde el ciclo en que tocaba (no deriva). La alarma sigue en tiempo
  real hasta la F2.4: programa un vistazo en el ciclo de su instante al ritmo del reloj detenido (en halt llega en
  hora; corriendo llega antes y vuelve a programar el resto); con el reloj detenido a 0 mira cada 16 384 ciclos.
  El paso detenido avanza evento a evento y para en el primero que levanta algo. Las instantáneas del depurador
  guardan `_cycles` y lo restauran antes que el Timer. Rendimiento, binarios alternados en la misma sesión: `mixed`
  137,8 frente a 139,1 de la F2.1 (−0,9 %). `loop` (3 instrucciones) y `branch` dependen de la alineación: sin la
  comprobación, `loop` sube a 180 pero `branch` baja a 151 (F2.1: 175 y 174; F2.2: 127 y 167). La F2.3 quita el
  bucle de `tick()`, que cuesta más que la comprobación.
  STDLIB: `test_game` y `test_debug_ansi` tenían cotas en instrucciones que a `-O0` se quedan cortas en ciclos
  (un marco vacío, 599; el bucle de `dbg_span`, 92 155): se amplían. La documentación de la STDLIB que aún habla
  de instrucciones (`timer.h`, `dbg_span_*` en `debug.h`) se reescribe en la F2.8.
- **F2.3**: sólo el DMA seguía con tick (entrada, audio y periféricos levantan desde hilos del host y no lo
  necesitaban). El DMA termina en su evento, un ciclo cada 8 bytes (mínimo 1). El halt salta al siguiente evento;
  sin ninguno espera al host (10 ms como mucho por paso, como antes). **Adelanta parte de la F2.4**: un halt que
  salta no puede convivir con una alarma en tiempo real (cada salto sumaba el tiempo que faltaba entero y el reloj
  se disparaba), así que los nanos y los milis del Timer ya son `_cycles` a 50 MHz (`DefaultCpuClockHz`, en
  `scheduler.h`), la alarma es un evento exacto en el primer ciclo en o tras su instante y `NanosResolution` da la
  duración de un ciclo (20 ns). `HaltClockRegister` (`0x1C`) da ahora esa frecuencia, que es lo que la STDLIB
  divide; la F2.4 lo sustituye por `CpuClockHz` y la hace configurable. El depurador ya no graba milis ni nanos
  (sólo el RTC y la entrada) y sus repeticiones no tocan ningún reloj. Hasta el `Pacer` (F2.5), un programa con
  ventana que se acompasa con halt va a toda velocidad. `test_halt_clock.cpp` pasa a `tests/e2e/test_scheduler.cpp`.
  `mixed` 137,3 frente a 136,0 de la F2.1 medidos alternados.
- **F2.4**: la tabla del Timer de SPEC §5.7 tal cual (`CyclesLow/High`, `Countdown` que se lee como lo que falta,
  `CountdownControl` con el bit periódico, `Nanos`, `Millis`, `Rtc`, alarma y `CpuClockHz`); desaparecen
  `NanosResolution` y el bit 31 de periódico. La alarma sigue en la IRQ 24: la SPEC le da la 17, que es del terminal
  hasta el mapa nuevo de la F4.3. `CpuClockHz` lo guarda el planificador (`Scheduler::clockHz`, 50 MHz por defecto)
  y lo leen el Timer (`0x28`) y SystemControl (`0x24`); la F2.5 lo hace configurable con `--cpu-clock`. RTC =
  arranque + tiempo virtual; el arranque es la hora del host al crear el Timer o `--rtc AAAA-MM-DDThh:mm:ss` (UTC,
  desde 1970). Tras un reset el motor ya está en el ciclo 0 cuando se reinician los dispositivos, así que el RTC
  vuelve a su arranque, como un encendido. Con el RTC determinista el depurador deja de grabar relojes: sólo graba
  la entrada (`History::clockValue` eliminado). `driver::MachineOptions` agrupa `strictMmio` y `rtc` (la F2.5 añade
  el ritmo). STDLIB: sólo los offsets nuevos y `timer_arm` con `CountdownControl`; `timer_nanos_resolution` sale de
  `CpuClockHz`. Su API y su documentación siguen para la F2.8.
- **F2.5**: `Pacer` (`driver/pacer.h`): ancla (instante del host, ciclo) y, entre tramos, duerme como mucho 10 ms
  mientras la máquina va por delante, para que el bucle siga bombeando la ventana; si va más de 250 ms por detrás
  (host lento, o una máquina parada esperando una tecla) suelta ese tiempo en vez de recuperarlo a toda prisa; un
  contador que retrocede (reset) vuelve a anclar. Mide la velocidad efectiva en ventanas de 500 ms y la pasa al
  backend (`HostBackend::reportSpeed`; la F5 la pondrá en la barra de estado). El bucle ejecuta hasta el siguiente
  milisegundo virtual, o hasta un halt o un fotograma de texto presentado (`FramebufferDevice::hasWindowFrame`),
  bombea y presenta la pantalla cuando hay fotograma nuevo o cada 16 ms; `instructionsPerFrame` desaparece. Sin
  `--speed`, `realtime` mientras la ventana está abierta (`HostBackend::windowOpen`) y `max` si no: así un programa
  de consola del build con SDL (las suites de Ceres-C) no se ralentiza. Sin ventana y con `max` se usa `vm.run()`
  tal cual. `--cpu-clock` (Hz, con k/M/G y Hz opcionales) va al planificador. Medido con `ceres run` de 2 s
  virtuales: `realtime` 2,07 s, `max` 55 ms, `4x` 570 ms. Un test en proceso con la entrada vacía descubrió que el
  cierre de la entrada levanta la IRQ del terminal y despierta un halt: los programas de prueba vuelven al halt
  mientras `Countdown` no lea 0.
- **F2.6**: `InputHub` (`driver/input_journal.h`) recibe la entrada de cualquier hilo del host y la aplica en el
  punto de inyección, que es cada vuelta del bucle: antes de cada tramo y tras el `pump` de la ventana. La sella con el
  ciclo y la graba tal como entró: los bytes del terminal, sólo los que caben en el anillo, así que el hub es ahora el
  control de flujo que hacían los hilos lectores. `HostBackend::pump` recibe un `InputSink` en vez de los dispositivos
  (el SDL sólo manda el mando cuando cambia); un archivo soltado es un evento más; cerrar la ventana se graba como
  `quit` y una repetición se para ahí; un reset se graba como `reset` y la repetición espera al suyo. Formato de texto,
  una línea por evento con las cadenas en hexadecimal, tras la cabecera `ceres-input 1`. Para que un halt sin eventos
  no espere 10 ms a una entrada que ya llegó, `InterruptController::poke` despierta la espera sin levantar nada (un
  raise terminaría el halt del programa y la repetición divergiría). Todo `ceres run` pasa ya por el bucle del driver:
  `vm.run()` ya no se usa ahí. `--replay` no lee la entrada del host.
- **F2.7**: `ceres profile` informa instrucciones y ciclos por función (una función va de una etiqueta global de
  `.text` a la siguiente; las locales se guardan como `función.etiqueta` y se agrupan con la suya) y por línea, con las
  cuotas sobre los ciclos. El motor cuenta los ciclos de cada palabra (`cycleCounts`) en un paso aparte y sin inline
  (`stepCounted`, macro `neverinline`): dentro de `step()` costaba un 9 % en `mixed` aun con el perfilado apagado.
  Medido alternando binarios, `mixed` 138,0 frente a 138,3 de la F2.3 y 139,5 de la F2.1. El depurador muestra `cycles`
  y el tiempo virtual en `regs`, el servidor los envía y las expresiones aceptan `cycles` y `nanos`; las instantáneas
  guardan la lista de eventos del planificador (`captureEvents`/`restoreEvents`) y la huella de los tests de historial
  compara ciclos, nanos y eventos. Un test comprueba que volver a antes de lanzar un DMA largo no deja su evento
  pendiente (falla sin la restauración). El estado del DMA sigue sin estar en las instantáneas.
- **F2.8**: macros con los nombres de la SPEC (`TIMER_CYCLES_LOW_REG`…`TIMER_CPU_HZ_REG`); `timer_cycles64()` sustituye a
  `timer_ticks64()` y `timer_cpu_hz()` a `timer_halt_clock()`; se retiran `timer_nanos()`, `timer_nanos_elapsed()` y
  `timer_wait_until_ns(struct ns64)` (`ns64.h` sigue hasta la F6); `timer_halt_until_ns` ya no mira si el host lleva
  tiempo real. Un tick es un ciclo: `timer_ticks`, `timer_elapsed`, `timer_arm`, `game` y `dbg_span` cuentan ciclos
  (`dbg_span_end` imprime «N cycles»). `clock()` ya eran nanos/1000 y ahora son tiempo virtual sin tocar el código.
  `.expected` cambiados a mano: sólo títulos de sección («instructions» → «cycles», y «not on a budget» en
  `test_game_halt`); `test_nanos` usa las formas `uint64_t` con las mismas 34 comprobaciones. Referencia regenerada.

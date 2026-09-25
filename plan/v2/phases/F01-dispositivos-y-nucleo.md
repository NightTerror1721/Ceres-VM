# F1 · Reorganización de dispositivos y del núcleo

- **Tamaño**: L · **Depende de**: F0 (F0.5 cerrada para P05 y P06) · **Repos**: CeresASM, Ceres-C, STDLIB
- **Objetivo**: un dispositivo por archivo con su `.h` y su `.cpp`, registros MMIO sólo de 32 bits, tabla de
  registros declarativa, un test por dispositivo, y decodificación de instrucciones independiente del host.
  **Salvo el acceso a MMIO de menos de 32 bits, no cambia el comportamiento.**
- **SPEC**: §5 (bus y dispositivos), §6.6 (codificación).

## Punto de partida (medido el 2026-09-25)

- `Ceres/libs/devices` es sólo cabeceras (`src/.gitkeep`). Contenido:
  - `devices.h` (1144 líneas): `SystemControlDevice` (l. 23), `TimerDevice` (l. 247), `TerminalDevice` (l. 624), `DmaController` (l. 1024).
  - `storage_devices.h`: `DiskDevice` (l. 34), `FramebufferDevice` (l. 247, que es la rejilla de texto).
  - `input_devices.h`: `KeyboardDevice` (l. 44), `MouseDevice` (l. 381), `GamepadDevice` (l. 479).
  - Un dispositivo cada uno: `audio_device.h`, `blitter_device.h`, `display_device.h`, `host_fs_device.h`, `peripheral_device.h`.
  - Auxiliares: `text_font.h`, `text_renderer.h`.
- `IODevice` y `MmioBus` están juntos en `Ceres/libs/vm/include/ceres/vm/mmio_bus.h`.
- Incluyen cabeceras de dispositivos: `libs/debug/include/ceres/debug/{debug_session.h,history.h}`,
  `libs/driver/include/ceres/driver/{host_backend.h,key_decoder.h}`, `libs/driver/src/{console_input.cpp,machine_runner.cpp}`,
  `libs/driver/tests/test_reset.cpp`, `libs/sdl/src/sdl_backend.cpp`, `libs/devices/include/ceres/devices/text_renderer.h`,
  `tests/e2e/{test_device_extras,test_devices,test_halt_clock,test_host_fs,test_macros,test_objects,test_peripherals,test_pipeline,test_text_window}.cpp`.
- Accesos de menos de 32 bits a MMIO fuera de CeresASM:
  - **Ceres-C**: 118 apariciones de `(char*)0xFF000004` (escribir un byte al terminal) en `examples/*.c` y
    `tests/e2e/test_e2e.cpp`, además de `examples/interop/io.h`, `libs/ir/tests/test_ir.cpp` y
    `libs/ir/tests/test_ir_optimizer.cpp`.
  - **STDLIB**: `include/ceres.h` define `mmio_r8`, `mmio_r16`, `mmio_w8` y `mmio_w16`; `src/ceres/sys.c` los usa en
    `read_port`/`write_port` según el tipo del puerto (`*_TYPE`); `src/ceres/terminal.c` escribe al terminal con
    `write_port(TERM_OUT, TERM_OUT_TYPE, …)`, que genera `strb` (`terminal.casm:135`).
  - **CeresASM**: los tutoriales `Ceres/examples/tutorial/*.casm`, `examples/main.casm`, `calling_convention.casm`,
    los fragmentos CASM de `tests/e2e` y la BIOS (`libs/vm/include/ceres/vm/bios.h`) pueden usar `strb`/`ldrb`
    sobre MMIO: revisa cada uno.

## Tareas

### F1.1 · Separar `IODevice` y añadir `RegisterMap`

- **Repos**: CeresASM · **Depende de**: —
- **Archivos**: crear `Ceres/libs/vm/include/ceres/vm/io_device.h` y `register_map.h`; modificar `mmio_bus.h`.
- **Pasos**:
  1. Mueve `IODevice` (y `DummyDevice`) de `mmio_bus.h` a `io_device.h`, sin cambiar su interfaz todavía;
     `mmio_bus.h` lo incluye.
  2. Crea `RegisterMap` (SPEC §5.3): `struct RegisterInfo { u32 offset; std::string_view name; Access access;
     u32 resetValue; std::string_view description; }` y `class RegisterMap { std::string_view device; std::span<const RegisterInfo> registers; const RegisterInfo* find(u32 offset) const; }`.
  3. Añade a `IODevice` `virtual const RegisterMap& registers() const` con una implementación por defecto que
     devuelve un mapa vacío (se hace obligatoria en F1.8).
- **Aceptación**: [x] Compila en todos los presets. [x] Suites en verde. [x] `detect_changes` sólo muestra los símbolos movidos.
- **Commit**: `Move IODevice to its own header and add RegisterMap (F1.1)`

### F1.2 · `libs/devices` como biblioteca compilada y con carpetas por grupo

- **Repos**: CeresASM · **Depende de**: F1.1
- **Archivos**: `Ceres/libs/devices/include/ceres/devices/{system,terminal,input,audio,storage,video}/` y los mismos
  en `src/`; `audio_device.h` → `audio/audio.h` + `audio/audio.cpp`; `blitter_device.h` → `video/blitter.h` + `.cpp`;
  `display_device.h` → `video/display.h` + `.cpp`; `host_fs_device.h` → `storage/host_fs.h` + `.cpp`;
  `peripheral_device.h` → `storage/peripherals.h` + `.cpp`; `text_font.h` → `video/default_font.h`;
  `text_renderer.h` → `video/text_renderer.h` + `.cpp`.
- **Pasos**:
  1. Mueve cada archivo con `git mv` para conservar la historia; pasa los cuerpos de las funciones al `.cpp`
     (dejando `inline` sólo lo trivial o crítico en rendimiento, y dilo en un comentario).
  2. Actualiza los `#include` de todos los usuarios (lista en «Punto de partida»).
  3. Deja `devices.h` como cabecera agregada que incluye todas las nuevas, para no romper a quien la use.
- **Aceptación**: [x] `libs/devices/src` tiene un `.cpp` por dispositivo movido. [x] Suites en verde.
- **Commit**: `Give each simple device its own folder and source file (F1.2)`

### F1.3 · Partir `devices.h`: sistema y terminal

- **Repos**: CeresASM · **Depende de**: F1.2
- **Archivos**: crear `system/system_control.{h,cpp}`, `system/timer.{h,cpp}`, `system/dma.{h,cpp}`,
  `terminal/terminal.{h,cpp}`; `devices.h` pasa a ser sólo la cabecera agregada.
- **Pasos**: como F1.2. Los nombres de clase no cambian todavía (`TerminalDevice` se sustituye en F5).
- **Aceptación**: [x] `devices.h` no define ninguna clase. [x] Suites en verde.
- **Commit**: `Split system control, timer, DMA and terminal into their own files (F1.3)`

### F1.4 · Partir `input_devices.h`

- **Repos**: CeresASM · **Depende de**: F1.2
- **Archivos**: `input/keyboard.{h,cpp}`, `input/mouse.{h,cpp}`, `input/gamepad.{h,cpp}`; borrar `input_devices.h`.
- **Aceptación**: [x] Suites en verde.
- **Commit**: `Split the input devices into their own files (F1.4)`

### F1.5 · Partir `storage_devices.h`

- **Repos**: CeresASM · **Depende de**: F1.2
- **Archivos**: `storage/disk.{h,cpp}`; `video/text_framebuffer.{h,cpp}` para `FramebufferDevice` (se elimina en F5);
  borrar `storage_devices.h`.
- **Aceptación**: [x] Ninguna cabecera de `libs/devices` define más de un `IODevice`. [x] Suites en verde.
- **Commit**: `Split disk and text framebuffer into their own files (F1.5)`

### F1.6 · Migrar a 32 bits todos los accesos a MMIO (tres repos)

- **Repos**: CeresASM, Ceres-C, STDLIB · **Depende de**: F1.5 · **Decisiones**: P05
- **Por qué va antes que F1.7**: los accesos de 32 bits ya funcionan en la VM actual, así que los tres repos se
  pueden migrar primero sin romper nada, y F1.7 sólo endurece la VM.
- **Pasos**:
  1. **CeresASM**: sustituye cada `strb`/`strh`/`ldrb`/`ldrh`/`ldrsb`/`ldrsh` sobre MMIO por `str`/`ldr` en
     tutoriales, ejemplos, fragmentos de `tests/e2e` y la BIOS. Para leer un byte de un registro, `ldr` y usa el
     byte bajo.
  2. **Ceres-C**: `(char*)0xFF000004` → `(volatile unsigned int*)0xFF000004` en `examples/*.c`,
     `examples/interop/io.h`, `tests/e2e/test_e2e.cpp`, `tests/e2e/test_prebuilt.cpp` y los tests de IR que lo
     mencionan; si algún test comprueba el código generado (un `strb`), actualiza su texto esperado.
  3. **STDLIB**: en `include/ceres.h`, elimina `mmio_r8`, `mmio_r16`, `mmio_w8`, `mmio_w16` y deja `mmio_r32`/`mmio_w32`;
     en `src/ceres/sys.c`, `read_port`/`write_port` siempre de 32 bits; quita los `*_TYPE` de puerto de las
     cabeceras que los definan; regenera los `.casm` de la librería (build normal).
- **Aceptación**: [ ] `grep` no encuentra accesos de menos de 32 bits a `0xFF......` en los tres repos. [ ] Las tres suites en verde.
- **Verificación**: las tres suites.
- **Commit** (uno por repo): `Reach every device register with a 32-bit access (F1.6)`

### F1.7 · Bus de 32 bits: `read`/`write` y `FaultReason`

- **Repos**: CeresASM · **Depende de**: F1.6 · **SPEC**: §5.1, §5.2, §5.4, §5.7 (SystemControl `0x2C`)
- **Archivos**: `io_device.h`, `mmio_bus.h`, `execution_engine.h` (rutas de lectura/escritura MMIO y `blockSpan`),
  todos los dispositivos, `system/system_control.{h,cpp}`, tests.
- **Pasos**:
  1. `IODevice` queda con `u32 read(Address)` y `void write(Address, u32)`. Elimina `readUnsignedByte`,
     `readSignedByte`, `readUnsignedHalfword`, `readSignedHalfword`, `readUnsignedWord`, `writeByte`,
     `writeHalfword` y `writeWord` de la interfaz y de cada dispositivo (cuidado: `object_file.cpp`,
     `history.cpp` y `debug_session.cpp` tienen funciones de nombre parecido que no son de `IODevice`).
  2. `MmioBus::read<T>`/`write<T>` sólo aceptan `T` de 4 bytes. En `ExecutionEngine`, un acceso a MMIO que no sea
     de 32 bits o esté desalineado lanza el fallo de SPEC §5.1 y deja el motivo en `FaultReason`; `ldrd`/`strd`
     aún no existen (F3). Las instrucciones de bloque sobre MMIO fallan con `MmioBlock`.
  3. Añade a SystemControl el registro `FaultReason` (`0x2C`) y conéctalo al motor como `FaultAddress`/`FaultAccess`.
  4. Tests nuevos: cada tipo de acceso prohibido da su fallo y su motivo; un `ldr`/`str` sigue funcionando.
- **Aceptación**: [ ] Ningún dispositivo implementa accesores de 8 o 16 bits. [ ] Tests de fallo pasan. [ ] `benchmark_vm` sin pérdida > 1 %.
- **Commit**: `Make device registers 32-bit only (F1.7)`

### F1.8 · Tabla de registros en cada dispositivo, `dev` y `--strict-mmio`

- **Repos**: CeresASM · **Depende de**: F1.7 · **Decisiones**: P06 · **SPEC**: §5.3
- **Pasos**:
  1. Cada dispositivo define su `RegisterMap` en su `.cpp` con todos sus registros; `registers()` pasa a ser
     virtual pura.
  2. Debugger: `dev` lista los dispositivos adjuntos; `dev <nombre>` muestra cada registro con nombre, acceso y
     valor actual (leer un registro con efectos, como una cola, no debe consumirla: usa un `peek` del estado).
  3. `ceres run --strict-mmio`: un offset no declarado da `MemoryFault` con motivo `MmioUndeclared`.
- **Aceptación**: [ ] Test que recorre todos los dispositivos y comprueba que su tabla no tiene offsets
  duplicados ni desalineados. [ ] Test de `--strict-mmio`.
- **Commit**: `Declare every device's registers in a table (F1.8)`

### F1.9 · Un test por dispositivo

- **Repos**: CeresASM · **Depende de**: F1.8
- **Archivos**: crear `Ceres/libs/devices/tests/test_<dispositivo>.cpp`; vaciar o borrar lo equivalente de
  `tests/e2e/test_devices.cpp` y `test_device_extras.cpp` (lo que sea de extremo a extremo con CASM se queda en e2e).
- **Aceptación**: [ ] Cada `.h` de dispositivo tiene su `test_*.cpp`. [ ] Mismo número o más de casos que antes.
- **Commit**: `Give each device its own test file (F1.9)`

### F1.10 · Campos de instrucción declarativos y serialización little-endian

- **Repos**: CeresASM · **Depende de**: — · **SPEC**: §6.6
- **Archivos**: `Ceres/libs/core/include/ceres/core/isa/instructions.h`, crear `isa/fields.h` y `base/endian.h`;
  `Ceres/libs/asm/include/ceres/asm/binary_emitter.h` (l. 122 y 133); `libs/vm/benchmarks/benchmark_vm.cpp`.
- **Pasos**:
  1. `Field<Pos, Width>` con `get`/`set` `constexpr`; define `Opcode`, `Rd`, `Rs`, `Rt`, `Imm8`, `Imm16`, `Imm20`,
     `Imm24` con él y reescribe los accesores de `Instruction` encima (misma interfaz pública).
  2. `loadLittleEndian32`/`storeLittleEndian32` en `endian.h` (con `std::endian` y `std::byteswap`).
  3. Sustituye `Instruction::asBytes` (las dos sobrecargas, que hacen `reinterpret_cast`) por funciones que
     escriben los bytes en little-endian en un búfer; corrige `binary_emitter.h:133` igual.
  4. Tests: `static_assert` por campo; ida y vuelta con vectores fijos; decodificar una secuencia de bytes
     invertida con `std::byteswap` para simular un host big-endian.
- **Aceptación**: [ ] Ningún `reinterpret_cast` sobre el valor de una instrucción. [ ] Mismo código generado en el
  bucle de ejecución (comprueba `benchmark_vm`).
- **Commit**: `Declare instruction fields in one table and serialize them little-endian (F1.10)`

### F1.11 · Comprobación automática de «un dispositivo por archivo»

- **Repos**: CeresASM · **Depende de**: F1.9
- **Archivos**: crear `Ceres/tests/cli/check_device_layout.cmake` (o un test C++ que recorra el árbol) y
  registrarlo en ctest.
- **Pasos**: el test falla si una cabecera de `libs/devices/include` declara más de una clase derivada de
  `IODevice`, si le falta su `.cpp` en `src/` o si le falta su `test_*.cpp`.
- **Aceptación**: [ ] El test pasa hoy y falla si se añade una segunda clase a un archivo (compruébalo a mano).
- **Commit**: `Check that every device keeps its own files (F1.11)`

### F1.12 · Documentación de dispositivos y bus

- **Repos**: CeresASM · **Depende de**: F1.8
- **Archivos**: crear `docs/35-Devices-and-Bus.md`; actualizar `docs/07-IO-Devices-and-Ports.md` y `docs/README.md`.
- **Pasos**: reglas de acceso (SPEC §5.1), `FaultReason`, `RegisterMap`, árbol de archivos, cómo añadir un dispositivo.
- **Commit**: `Document the device layout and the 32-bit bus (F1.12)`

## Cierre de la fase

- [ ] Suites en verde en los tres repos. [ ] `benchmark_vm` dentro del 1 % de `BASELINE.md`. [ ] Revisión `ocr`.

## Notas

- **Riesgo**: GitNexus da HIGH en `IODevice` (21 dependientes directos: todos los dispositivos), y dará HIGH en
  `MmioBus` y en las rutas MMIO del motor. El usuario autorizó el 2026-09-25 seguir con los HIGH de los símbolos que
  nombra cada tarea de F1, y parar sólo ante un HIGH o CRITICAL fuera de lo previsto.
- **F1.1**: `IODevice`, `DummyDevice`, `NoDeviceEvent` y `DefaultHaltClockHz` pasan tal cual a `io_device.h`;
  `registers()` devuelve una tabla vacía por defecto. `RegisterAccess` es el nombre del enum de acceso (evita chocar
  con `AccessKind` y `MmuAccess`). Verificado con GCC, MSVC y el binario SDL.
- **F1.2**: los cuerpos se movieron con un separador en Python (un solo uso, no está en el repo) que se apoya en el
  estilo del código: firma, `{` y `}` en su propia línea a la sangría del miembro. Quedan en la cabecera los cuerpos
  de una línea, lo `constexpr`, lo `forceinline`, las plantillas y los métodos de clases anidadas. Git sigue viendo
  los renombrados salvo `host_fs.h`, que al quedarse casi sin cuerpo cae por debajo del 50 % de parecido:
  `git log --follow -M30% -- Ceres/libs/devices/include/ceres/devices/storage/host_fs.h` sigue su historia. Las
  referencias a los nombres viejos en `docs/01-Overview.md` y `docs/07-IO-Devices-and-Ports.md` se rehacen en F1.12.

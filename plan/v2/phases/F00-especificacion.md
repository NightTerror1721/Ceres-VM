# F0 · Especificación y medición

- **Tamaño**: S · **Depende de**: — · **Repos**: CeresASM (y el usuario)
- **Objetivo**: medir el punto de partida, probar lo arriesgado en prototipos aparte y cerrar con el usuario las
  decisiones pendientes que bloquean las primeras fases. No cambia el comportamiento de la máquina.

## Tareas

### F0.1 · Medir el intérprete por clase de instrucción

- **Repos**: CeresASM · **Depende de**: — · **SPEC**: §3.2
- **Archivos**: modificar `Ceres/libs/vm/benchmarks/benchmark_vm.cpp`; crear `plan/v2/BASELINE.md`.
- **Pasos**:
  1. Añade al benchmark bucles que ejerzan cada clase de la tabla §3.2 por separado (ALU, carga/almacenamiento
     en RAM, MMIO, saltos, `call`/`ret`, `mul`, `div`, float, `fdiv`, bloque) y un programa mixto realista.
  2. Compila con el preset `gcc-ipo` en Release y ejecútalo tres veces.
  3. Apunta en `BASELINE.md`: host (CPU, SO), compilador, MIPS por clase (mediana), MIPS del mixto.
- **Aceptación**:
  - [x] `BASELINE.md` tiene las medidas y el comando exacto para repetirlas.
  - [x] El benchmark sigue compilando en todos los presets (GCC sí; MSVC y clang, ver «Notas»).
- **Verificación**: `ctest` de CeresASM en verde; ejecutar el benchmark.
- **Commit**: `Measure the interpreter per instruction class (F0.1)`

### F0.2 · Medir la presentación y el arranque con memoria grande

- **Repos**: CeresASM · **Depende de**: F0.1
- **Archivos**: `plan/v2/BASELINE.md`.
- **Pasos**:
  1. Con `ceres.exe` (SDL) y un programa que presenta píxeles en bucle, mide cuántas veces por segundo se llama a
     `SdlBackend::present` y cuántos MIPS pierde la VM frente a `CERES_HEADLESS=1` (añade un contador temporal
     o usa un perfilador; no lo subas).
  2. Mide tiempo de arranque y memoria residente del proceso con `--memory` de 16 MiB y 1 GiB (el máximo actual).
- **Aceptación**: [x] Números en `BASELINE.md`, secciones «Presentación» y «Memoria».
- **Verificación**: ninguna de código (no hay cambios de código subidos).
- **Commit**: `Record the present and memory baseline (F0.2)`

### F0.3 · Prototipo de SDL_GPU

- **Repos**: CeresASM · **Depende de**: —
- **Archivos**: crear `spikes/sdl_gpu/` en la raíz del repo (fuera de `Ceres/`, fuera del build normal) con un
  `CMakeLists.txt` propio y un `README.md`.
- **Pasos**:
  1. Programa mínimo: `SDL_CreateGPUDevice` (SPIR-V, DXIL, MSL), `SDL_ClaimWindowForGPUDevice`, un quad
     texturizado con un shader precompilado, una subida con copy pass y una descarga con `SDL_DownloadFromGPUTexture`.
  2. Anota qué backends arrancan en la máquina de desarrollo y si SDL permite forzar WARP (D3D12) o lavapipe
     (Vulkan) para CI.
  3. Anota qué herramienta compila los shaders (SDL_shadercross, glslang, DXC) y cómo se incrustan.
- **Aceptación**: [ ] El spike compila y dibuja; `spikes/sdl_gpu/README.md` recoge backends, CI y cadena de shaders.
- **Verificación**: compilarlo y ejecutarlo a mano.
- **Commit**: `Add an SDL_GPU spike outside the build (F0.3)`

### F0.4 · Prototipo del terminal ANSI y de un operador FM

- **Repos**: CeresASM · **Depende de**: —
- **Archivos**: `spikes/vterm/`, `spikes/fm/` (fuera del build).
- **Pasos**:
  1. `vterm`: interpreta la salida real de dos ejemplos de la STDLIB que usan `tui.h` y `ansi.h` con el
     subconjunto ANSI de SPEC §8.2 y comprueba que no queda ninguna secuencia sin cubrir. Anota las que falten.
  2. `fm`: un operador FM de 4 operadores que genera un WAV de una nota, para validar el coste por muestra.
- **Aceptación**: [ ] Lista de secuencias ANSI que emite la STDLIB, con las no cubiertas señaladas; WAV de prueba.
- **Commit**: `Add terminal and FM spikes outside the build (F0.4)`

### F0.5 · Cerrar decisiones con el usuario · `HUMANO`

- **Repos**: CeresASM (plan) · **Depende de**: F0.1, F0.2
- **Decisiones**: P01, P03, P04, P05, P06, P07, P08, P09, P10, P11
- **Pasos**:
  1. Presenta al usuario cada decisión con su opción por defecto y los datos de `BASELINE.md` que la afecten.
  2. Pasa las respuestas a «Cerradas» en `DECISIONS.md` y actualiza `SPEC.md`.
- **Aceptación**: [ ] Ninguna de esas decisiones queda pendiente.
- **Commit**: `Close the pending decisions for the first phases (F0.5)`

### F0.6 · Calibrar la tabla de ciclos · `HUMANO`

- **Repos**: CeresASM (plan) · **Depende de**: F0.1 · **Decisiones**: P02
- **Pasos**:
  1. Con las medidas de F0.1, comprueba que `standard` (50 MHz) se sostiene en tiempo real con margen (el mixto
     necesita menos del 70 % de los MIPS medidos) y que los costes relativos son razonables.
  2. Propón al usuario la tabla ajustada; al aceptarla, pasa SPEC §3.2 a NORMATIVA y cierra P02.
- **Aceptación**: [ ] SPEC §3.2 NORMATIVA; P02 cerrada.
- **Commit**: `Settle the cycle table (F0.6)`

### F0.7 · Enlazar el plan desde la documentación

- **Repos**: CeresASM · **Depende de**: —
- **Archivos**: `docs/README.md`, `docs/29-SDL3-Integration-Plan.md`.
- **Pasos**: enlaza `plan/v2/README.md` desde el índice de docs; marca el doc 29 como histórico, sustituido por
  el plan v2.
- **Aceptación**: [ ] Enlaces correctos.
- **Commit**: `Point the docs at the v2 plan (F0.7)`

## Notas

(El agente apunta aquí medidas, hallazgos y desvíos del plan.)

- **F0.1**: medidas en [BASELINE.md](../BASELINE.md). El `mixed` da 134 MIPS; con la tabla provisional (1,61 ciclos
  por instrucción) `standard` usa el 23 % del host y `workstation` el 46 %; código sólo ALU en `workstation`, el 71 %.
  El benchmark acepta nombres como argumentos para ejecutar sólo esos.
- **F0.1, MSVC**: el preset `msvc` (MSVC 14.50, VS 18) no compilaba la VM: `MmioBus::attach`, `attachRange` y
  `detach` eran `constexpr` con un `std::lock_guard` dentro (C3615). Arreglado en `asm@4d56058`. Con eso salió
  una carrera en el driver: el lector de un `istream` que no es `std::cin` iba en un hilo desacoplado y podía leer
  el stream del test ya destruido, y el test de entrada por tubería esperaba mirando un bit que siempre está a 1.
  Arreglado en `asm@ecb5aa2` (0 fallos en 20 ejecuciones con MSVC y con GCC; antes 2 de 20 con GCC). No hay clang
  instalado en la máquina de desarrollo; el preset `clang` no se ha probado. Tras un cambio de cabeceras,
  `build/msvc` puede quedarse con objetos viejos: si un test revienta sólo ahí, `--clean-first`.
- **F0.2**: la ventana presenta cada 4096 instrucciones sin vsync (6 400 imágenes/s a 320×240) y eso le quita a
  la VM el 76 % (320×240) o el 94 % (1280×720) de los MIPS; `pump` no cuesta. La RAM se reserva y se pone a cero
  entera: 1 GiB son 138 ms de arranque y 1 GiB residente. Detalle en [BASELINE.md](../BASELINE.md).

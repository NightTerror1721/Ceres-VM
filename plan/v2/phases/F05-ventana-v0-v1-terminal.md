# F5 · La ventana es el ordenador: GPU V0–V1 y terminal virtual

- **Tamaño**: XL · **Depende de**: F4, decisiones P03, P07, P08, P09 · **Repos**: CeresASM, Ceres-C, STDLIB
- **Objetivo**: toda la E/S del programa pasa a la ventana SDL. Nace la GPU con los niveles V0 (texto) y V1
  (bitmap), el terminal virtual sustituye al terminal del host y, sin ventana, los resultados van a archivos.
  Al terminar, la máquina nueva hace todo lo que hacía la v1. **Hito 1.**
- **SPEC**: §7.1–§7.4, §8, §10, §11.

## Punto de partida

- `libs/driver/include/ceres/driver/host_backend.h`: `HostBackend` (pump, present, presentText, openWindow, audio,
  archivos soltados, `instructionsPerFrame`) y `HeadlessBackend`.
- `libs/sdl/src/sdl_backend.cpp`: ventana, `SDL_Renderer`, texto con `TextRenderer`, teclado, ratón, gamepad, audio.
- `libs/driver/src/console_input.{h,cpp}`: teclas de la consola del host.
- `video/text_framebuffer` (antes `FramebufferDevice`), `video/display` (antes `DisplayDevice`), `video/text_renderer`.
- `TerminalDevice` en `terminal/terminal.{h,cpp}`: flujo con el stdio del host.
- STDLIB: `terminal.h`, `textfb.h`, `display.h`, `tui.h`, `ansi.h`, `gfx.h`, `game.h`; `tools/runtests.ps1` y
  `runtests.js` comparan la salida estándar con `tests/expected/*.expected` (103), alimentan `*.stdin` (10) y
  comparan `*.stderr` (4); `tools/consolecheck.ps1` y `tools/console/` prueban teclas en una consola real.
- Ceres-C: `tests/framework/ceres_tool.h` ejecuta programas y lee su stdout; `tests/examples` y `tests/e2e` lo usan.

## Tareas

### F5.1 · Interfaces del host y división del backend SDL (sin cambiar comportamiento)

- **Repos**: CeresASM · **Depende de**: —
- **Archivos**: crear en `libs/driver/include/ceres/driver/`: `host_input.h`, `video_output.h`, `audio_output.h`,
  `host_log.h` (ya existe desde F4.4: reutilízalo); en `libs/sdl`: `sdl_window.{h,cpp}`, `sdl_presenter.{h,cpp}`,
  `sdl_audio.{h,cpp}`; borrar `host_backend.h` y `sdl_backend.{h,cpp}` al final.
- **Pasos**: reparte las responsabilidades de `HostBackend` entre las interfaces y adapta `machine_runner.cpp`.
  El comportamiento visible no cambia.
- **Aceptación**: [ ] Suites en verde. [ ] La ventana, el audio y el gamepad funcionan igual (prueba manual con dos ejemplos).
- **Commit**: `Split the host backend into input, video, audio and log (F5.1)`

### F5.2 · Núcleo de la GPU, pantalla y VBlank

- **Repos**: CeresASM · **Depende de**: F5.1 · **SPEC**: §7.3 (`0x000–0x120`), §5.6 (IRQ 32–35)
- **Archivos**: crear en `libs/devices/.../video/`: `gpu.{h,cpp}` (el `IODevice` de los slots `0x40–0x43`),
  `display_controller.{h,cpp}`, `gpu_executor.h` (interfaz), `software_executor.{h,cpp}`, `formats.h`, `cost_model.h`;
  tests.
- **Pasos**:
  1. Registros del núcleo y de pantalla con su tabla; `Mode` limitado por `MaxLevel`; `GpuClockHz`.
  2. Temporización de vídeo en el planificador: VBlank (IRQ 32), `VCount`, `LineCompare` (IRQ 33), `FrameCounter`.
  3. `Present` aplica las bases pendientes en el siguiente VBlank.
  4. `GpuExecutor` con una sola implementación, `SoftwareExecutor`, que compone el fotograma.
- **Aceptación**: [ ] Test: VBlank a la frecuencia exacta en ciclos (con el resto acumulado). [ ] Test de `LineCompare`.
- **Commit**: `Add the GPU core, the display controller and VBlank (F5.2)`

### F5.3 · Plano de texto (V0)

- **Repos**: CeresASM · **Depende de**: F5.2 · **SPEC**: §7.3 (`0x200–0x23F`), §7.4
- **Archivos**: `video/text_plane.{h,cpp}`; `video/default_font.h` (desde `text_font.h`, convertida a 8×16, 1 bpp,
  256 glifos); tests.
- **Pasos**: celdas de 16 y 32 bits; fuente y paleta leídas de la VRAM; cursor; disposición de VRAM al arrancar
  (SPEC §7.4) publicada en los registros; composición en el `SoftwareExecutor`.
- **Aceptación**: [ ] Test que escribe celdas y compara el hash del fotograma compuesto.
- **Commit**: `Add the text plane (F5.3)`

### F5.4 · Plano bitmap y motor de copia (V1)

- **Repos**: CeresASM · **Depende de**: F5.3 · **SPEC**: §7.1 (V1), §7.3 (`0x240–0x2BF`)
- **Archivos**: `video/bitmap_plane.{h,cpp}`, `video/copy_engine.{h,cpp}`; tests.
- **Pasos**: formatos I1–ARGB8888 con paleta en VRAM; pitch; scroll; 1–3 búferes con flip en VBlank; motor de
  copia (copy y fill) con coste en ciclos de GPU e IRQ 34.
- **Aceptación**: [ ] Tests de cada formato (hash). [ ] Test de coste: una copia termina en el ciclo esperado.
- **Commit**: `Add the bitmap plane and the copy engine (F5.4)`

### F5.5 · Presentación por VBlank y ventana

- **Repos**: CeresASM · **Depende de**: F5.4 · **Decisiones**: P09
- **Pasos**:
  1. El runner pide un fotograma a la GPU en cada VBlank y se lo da a `VideoOutput`; se acabó `present` por rebanada.
  2. `SdlPresenter`: textura streaming, escala entera, letterbox; `--fullscreen` y F11.
  3. Barra de estado (título de la ventana o franja): perfil, velocidad efectiva, `[terminado: código N]`.
  4. La ventana se queda abierta al terminar; `--exit-on-halt` la cierra.
  5. Sin ventana: `--frames <dir>` escribe un PNG por `Present`.
- **Aceptación**: [ ] Medida en «Notas»: presentaciones por segundo = refresco. [ ] Prueba manual de la ventana.
- **Commit**: `Present one frame per VBlank and keep the window after the program ends (F5.5)`

### F5.6 · Terminal virtual y pantalla de fallo

- **Repos**: CeresASM · **Depende de**: F5.3 · **Decisiones**: P08 · **SPEC**: §8
- **Archivos**: reescribir `terminal/terminal.{h,cpp}`; crear `terminal/ansi_parser.{h,cpp}`,
  `terminal/line_discipline.{h,cpp}`, `terminal/scrollback.{h,cpp}`; tests por archivo.
- **Pasos**:
  1. Registros de SPEC §8.1 con su tabla.
  2. Salida: UTF-8 a glifos, controles y subconjunto ANSI de §8.2 (usa la lista de F0.4), escribiendo celdas en el
     plano de texto; salida de error en su color.
  3. Entrada: texto del teclado de la ventana, disciplina de línea de §8.3, Ctrl+C con IRQ 19, Ctrl+D.
  4. Scrollback con Shift+RePág/AvPág.
  5. Pantalla de fallo: la BIOS y el motor, ante una excepción sin manejador, pintan en el plano de texto el
     nombre de la excepción, PC, dirección, acceso, motivo y (si hay tabla de símbolos) la pila; lo mismo va a `HostLog`.
- **Aceptación**: [ ] Test por secuencia ANSI. [ ] Test de la disciplina de línea con entrada guionizada. [ ] Test de la pantalla de fallo (hash del texto).
- **Commit**: `Replace the host terminal with a virtual terminal in the window (F5.6)`

### F5.7 · Salidas sin ventana y entrada guionizada

- **Repos**: CeresASM · **Depende de**: F5.6 · **SPEC**: §10
- **Archivos**: crear `libs/driver/src/headless_output.{h,cpp}`; `apps/cli`; borrar `libs/driver/src/console_input.{h,cpp}`
  y `libs/driver/tests/test_key_decoder.cpp` si ya no aplica.
- **Pasos**: `--headless`, `--transcript` (salida con los bytes de error entre `\x1b[E` y `\x1b[e`),
  `--screen-log`, `--type`, `--keys`; elimina `--terminal` y todo camino que escriba la salida del programa en el
  stdout del host o lea su stdin.
- **Aceptación**: [ ] Test: un programa que hace `printf` no escribe nada en el stdout del proceso `ceres`, y el
  transcript tiene su salida.
- **Commit**: `Send headless output to files and script the keyboard (F5.7)`

### F5.8 · Retirar los dispositivos de texto y píxeles antiguos

- **Repos**: CeresASM · **Depende de**: F5.5, F5.7
- **Pasos**: borra `video/text_framebuffer`, `video/display`, `video/text_renderer` y sus tests; libera los slots
  temporales `0x44` y `0x45`; actualiza e2e.
- **Commit**: `Remove the v1 text framebuffer and pixel display (F5.8)`

### F5.9 · Ceres-C: tests y ejemplos sin stdout del host

- **Repos**: Ceres-C · **Depende de**: F5.7
- **Archivos**: `tests/framework/ceres_tool.h` y sus usuarios (`tests/e2e`, `tests/examples`), `README`/docs si
  describen la ejecución.
- **Pasos**: ejecutar los programas con `--headless --speed max --transcript <tmp>` y leer el transcript en lugar
  del stdout; la entrada, con `--type`.
- **Aceptación**: [ ] `ctest` de Ceres-C en verde.
- **Commit**: `Read program output from the transcript (F5.9)`

### F5.10 · STDLIB: cabeceras de vídeo y terminal

- **Repos**: STDLIB · **Depende de**: F5.8
- **Archivos**: crear `include/ceres/video.h`, `include/ceres/text.h`, `include/ceres/fb.h` y sus fuentes; adaptar
  `terminal.h`, `tui.h`, `gfx.h` (dibuja en el back buffer de VRAM), `game.h` (ritmo por VBlank), `stdio` si
  hace falta; borrar `textfb.h` y `display.h`; regenerar `docs/reference`.
- **Aceptación**: [ ] Ningún archivo usa `textfb.h` ni `display.h`. [ ] La librería compila en todos los niveles.
- **Commit**: `Add video, text and framebuffer headers on the new GPU (F5.10)`

### F5.11 · STDLIB: tests y ejemplos sin ventana

- **Repos**: STDLIB · **Depende de**: F5.10, F5.9
- **Archivos**: `tools/runtests.ps1`, `tools/runtests.js`, la integración con `ctest` del `CMakeLists.txt`,
  `tests/expected/*`, `examples/expected/*`, `tests/*.c` y `examples/*.c` que lo necesiten; borrar
  `tools/consolecheck.ps1` y `tools/console/`.
- **Pasos**:
  1. Ejecutar con `--headless --speed max --gpu software --transcript`; `.stdin` → `--type`; `.stderr` se
     compara con la parte de error del transcript; tests de `text.h`/`tui.h` con `--screen-log`.
  2. `-Update` y revisión a mano de cada `.expected` que cambie.
  3. Migrar los ejemplos (pong, snake, life, mandelbrot, rps_tui, calc, guess…).
- **Aceptación**: [ ] `runtests.ps1` en verde. [ ] Ningún test escribe en el stdout del host.
- **Commit**: `Run the library tests headless through the transcript (F5.11)`

### F5.12 · Puerta «host limpio» y documentación

- **Repos**: CeresASM, STDLIB · **Depende de**: F5.11
- **Pasos**: test que ejecuta todos los ejemplos de los tres repos sin ventana y falla si alguno escribe en el
  stdout del host; docs `31-Video.md` (V0–V1), `33-Terminal-and-Debug-Log.md`, `07`; README de la STDLIB.
- **Commit** (uno por repo): `Check that programs never write to the host terminal and document it (F5.12)`

## Cierre de la fase

- [ ] Suites en verde en los tres repos. [ ] Puerta «host limpio». [ ] Revisión `ocr`. [ ] Hito 1 anunciado al usuario.

## Notas

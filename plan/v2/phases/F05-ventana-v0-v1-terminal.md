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

- **F5.1**: las interfaces son `HostInput` (bombeo y archivos soltados), `VideoOutput` y `AudioOutput`; un host con
  ventana es un `WindowHost` con las tres (en `shared_ptr`, porque la ventana SDL es a la vez la entrada y lo que
  dibuja el presentador) y lo crea una `WindowHostFactory`. En `libs/sdl`, `sdl_window`, `sdl_presenter` y
  `sdl_audio` son privados (`src/`) y el único público es `sdl_host.h` (`createSdlHost()`). Prueba manual: `pong`
  abre su ventana al tamaño de siempre; no se capturó el contenido (PrintWindow no ve el renderer de D3D y el PC
  estaba en uso), queda para la prueba de la ventana de F5.5.
- **F5.2**: `display_controller.h` es sólo cabecera (aritmética pura). La GPU arranca a 640×480 (80×30 celdas) o a
  la resolución máxima del perfil si es menor. Sólo se adjunta el slot `0x40`: `0x41–0x43` no se adjuntan hasta las
  fases que los llenan (el dispositivo no distingue por qué slot se le accede). El evento de línea sólo existe con
  `IrqEnable` bit 1. La GPU aún no está en el runner: entra en F5.5, con la presentación (con ella, una máquina
  detenida salta de VBlank en VBlank en lugar de esperar al host).
- **F5.3**: la fuente de 8×16 se genera en `default_font.h` (constexpr) desde la tabla 5×7, como la dibujaba la
  ventana; `0x80–0x9F` llevan caja y bloques. Cols y Rows son de sólo lectura (Width/8, Height/16). Se añaden
  `ScrollbackHead` (`0x238`) y `ScrollbackCount` (`0x23C`) para el anillo del scrollback, que el terminal (F5.6)
  llena. Un fondo 0 es transparente. `CursorShape` bit 8 parpadea cada 16 fotogramas. SPEC §7.1 y §7.3 recogen la
  celda de 32 bits, la fuente, la paleta y los offsets.
- **F5.4**: se añade `SpareBase` (`0x26C`), la dirección del tercer búfer. Los formatos de menos de 8 bits guardan el
  píxel de la izquierda en los bits altos del byte; los que tienen alfa se mezclan sobre lo de debajo con redondeo
  entero. Al arrancar, el plano está apagado, en XRGB8888 al tamaño de la pantalla, con tres búferes preparados tras
  el scrollback si caben en la VRAM y la paleta del texto. El motor de copia mueve los bytes al terminar (no a
  medias) y un relleno alinea su patrón a la dirección, como lo haría un `str`.
- **Revisión `ocr` F5.1–F5.4** (modo delegado: el diff revisado a mano con las reglas de `ocr delegate rule`): un
  hallazgo real, corregido: al partir el backend nadie llamaba ya a `SDL_Quit` al cerrar el último subsistema. Queda
  apuntado para F5.6: el historial del debugger no guarda el estado interno de la GPU (fotograma, planos), sólo sus
  eventos; y con `custom` una VRAM mínima no cabe el terminal de 1920×1080, así que el texto no se ve.
- **F5.5**: `VideoOutput` pasa a `openWindow(w, h)`, `present(VideoFrame)`, `setFullscreen` y `setStatus` (la barra
  de estado es el título de la ventana: perfil, velocidad y `[terminado: código N]`). La GPU entra en el runner con
  la configuración del perfil y `--refresh`; en cada VBlank compone y la ventana recibe el fotograma, como mucho uno
  cada 8 ms del host (a `--speed max` no se dibuja cada VBlank). `--frames` compone en el propio VBlank del
  `Present`, así que los PNG son los mismos en cada ejecución (PNG RGB sin compresión, escrito sin bibliotecas).
  La ventana se abre al arrancar a la resolución de la GPU; sin pantalla, la máquina sigue sin ventana salvo con
  `--window`. Al terminar se queda abierta hasta una tecla o cerrarla (`--exit-on-halt` la cierra). F11 alterna
  pantalla completa y no llega al programa. Mientras duren (hasta F5.8), el framebuffer de texto de la v1 va
  siempre al terminal y el display de píxeles ya no se ve en la ventana: `pong.casm` no se ve hasta migrarlo en
  F5.8. Se deja de capturar el ratón (se hacía al ver píxeles del display). **Medida**: con `--speed realtime`, 30
  VBlanks a 60 Hz tardan medio segundo y llegan a la ventana 29–30 presentaciones (test
  `driver_screen/a_window_shows_one_frame_a_vertical_blank_in_real_time`): presentaciones por segundo = refresco.
- **F5.6**: el terminal dibuja en el plano de texto de la GPU (`setScreen`); sin pantalla (tests, el `Machine` del
  driver) sigue funcionando sólo como flujos. Salida y error tienen cada uno su parser y su «pluma», así que una
  secuencia partida en uno no la termina el otro, y `SGR 0` en el error vuelve al color de error (tinta 9). Con
  celdas de 16 bits, `38;5;n`/`48;5;n` usan el más cercano de los 16 primeros; con las de 32 bits, el índice tal cual.
  El salto de línea al final de la fila espera al carácter siguiente (como xterm). `\a` no suena hasta que exista el
  tono A0 (F9). Las filas que suben por la pantalla completa van al scrollback; las de una región de scroll, no.
  Entrada: la cola del programa es de 8 KiB (antes, anillo de 64 bytes); lo tecleado (`typeKeystroke`, `type`) pasa
  por la disciplina de línea, y `pushInput` entra directo, como una tubería (el debugger, los datos de stdin hasta
  F5.7). Ctrl+D con la línea vacía marca el fin de entrada hasta que llegue más texto; `closeInput` es definitivo y
  entrega la línea a medias. El teclado sigue ahora Ctrl y Shift: Ctrl+letra da el carácter de control (Ctrl+C = 3,
  que SDL no manda como texto) y RePág/AvPág con Shift llevan `KeyShift` (bit 30), que el terminal usa para el
  scrollback. La pantalla de fallo pinta en blanco sobre rojo el mismo informe que va a `HostLog` (excepción, PC,
  acceso, dirección, motivo); **la pila no se pinta**: recorrerla sin tabla de símbolos ni marcos fiables daría
  basura, y la STDLIB ya tiene `ceres/backtrace.h` para el programa. El comando de bloque cambia de número (1
  escribir, 2 leer, 3 error): se actualizaron los ejemplos y los tests e2e. La sesión del debugger tiene ahora GPU
  (el terminal dibuja en ella); su estado interno no está en las instantáneas del historial (su VRAM sí). Prueba
  manual: `01_hola` en la ventana muestra el texto, el cursor y el título `[terminado: código 0]`.
- **F5.7**: `--headless` sustituye a `--terminal` (que ahora es una opción desconocida); `--window` se queda y
  choca con `--headless`. `--screen-log` escribe cada pantalla tras una línea `--- present <n> ---` y la última tras
  `--- end ---`. `--type` pasa por la disciplina de línea (con eco en pantalla, que no va al transcript); `--keys`
  lee líneas `<ms> down|up|press <tecla>` o `<ms> text <texto>` (archivo `key_script.{h,cpp}`) y el hub las inyecta
  en su ciclo, junto a la entrada del host; los dos quedan en `--record`. Sin ventana, la entrada del terminal se
  cierra tras lo guionizado (enseguida si no hay nada); con `--replay`, lo que diga la grabación. Se borran
  `console_input.{h,cpp}`, `key_decoder.h` y su test: el runner no lee el stdin del host ni escribe en su stdout (los
  frames del framebuffer de texto de la v1 se descartan hasta F5.8). Test de aceptación en `tests/cli`:
  `headless_output` ejecuta el binario y comprueba stdout y stderr vacíos y el transcript exacto; `determinism` lee
  ya el transcript. Los tests del driver leen la salida con `run_capture.h`.

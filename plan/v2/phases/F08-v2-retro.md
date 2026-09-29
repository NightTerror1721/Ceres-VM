# F8 · V2: Retro 2D

- **Tamaño**: L · **Depende de**: F5, decisión P04 (cerrada: D17) · **Repos**: CeresASM, STDLIB
- **Objetivo**: tiles, sprites y efectos raster con color indexado, programados por registros y tablas en VRAM,
  como las consolas de 8 y 16 bits. Todo en el `SoftwareExecutor` (el hardware llega en F12).
- **SPEC**: §7.1 (V2), §7.2, §7.3, §7.5 (`0x300–0x3FF` y los formatos en VRAM) y §7.6 (scanout por líneas), que esta
  fase fija y pasa a NORMATIVA.

## Antes de empezar

Hecho en el commit `plan: fix the V2 register layout`: SPEC §7.5 fija `0x300–0x3FF` (paletas, sprites, tabla de
líneas, cuatro capas de `0x20` bytes y la capa afín), la entrada de mapa de 16 bits (tile 9:0, paleta 12:10,
volteos 13 y 14, prioridad 15), la entrada de OAM de 16 bytes, la tabla de líneas, qué pasa con una escritura en
VRAM fuera del VBlank en `micro` y `pocket` (`FaultCode` 2) y §7.6, el scanout por líneas.

## Punto de partida

- La GPU (`libs/devices/src/video/gpu.cpp`) tiene V0 y V1 (`ImplementedLevel = 1`); sus partes internas son
  `text_plane`, `bitmap_plane`, `copy_engine` y `display_controller`, y el `SoftwareExecutor` compone el fotograma
  entero con el estado del momento (`GpuDevice::compose`), que el runner llama en el observador del VBlank.
- `MachineProfile::spritesPerLine` ya existe (`driver/profiles.h`) pero no llega a la GPU.
- Las escrituras de la CPU en VRAM pasan por `ExecutionEngine::writeOutsideRam` y, las de bloque, por
  `vramBlockSpan`; la DMA, el motor de copia y el terminal escriben la VRAM por su cuenta.

## Tareas

### F8.1 · Paletas y capas de tiles
- **Repos**: CeresASM · **Depende de**: F5 y el commit de «Antes de empezar»
- **Archivos**: nuevos `devices/video/retro2d.{h,cpp}` (el bloque de registros de V2: paletas, capas y, en las
  tareas siguientes, la afín, los sprites y la tabla de líneas) y `devices/video/retro_scanout.{h,cpp}` (la línea
  de V2 en software); `video/gpu.{h,cpp}` (`ImplementedLevel` 2, `0x300–0x3FF`), `video/gpu_executor.h`
  (`ScanoutState::retro`), `video/software_executor.cpp`; test `devices/tests/test_tile_layers.cpp`.
- **Pasos**: 16 paletas de 16 y una de 256 en VRAM (TilePaletteBase); 4 capas (mapa, tileset, tamaño de mapa 32–128,
  tile 8×8 o 16×16, 4 u 8 bpp, scroll, prioridad, banco de paleta, tabla de scroll por línea).
- **Aceptación**: [x] Tests por hash con cada combinación de tamaño y bpp, contra un modelo de referencia del test.
- **Commit**: `Add palettes and tile layers (F8.1)`

### F8.2 · Capa afín
- **Archivos**: `video/retro2d.{h,cpp}`, `video/retro_scanout.cpp`; test `devices/tests/test_affine_layer.cpp`.
- **Pasos**: una capa con matriz 2×2 en 8.8 y origen en 24.8; modo de vuelta o de recorte.
- **Commit**: `Add the affine layer (F8.2)`

### F8.3 · Sprites y VRAM en el VBlank (D17)
- **Archivos**: `video/retro2d.{h,cpp}`, `video/retro_scanout.{h,cpp}` (evaluación de sprites por línea, que usa
  también `SpriteStatus`), `video/gpu.{h,cpp}` (`Config::spritesPerLine` y `vramInVblankOnly`, `FaultCode` 2),
  `vm/vram.h` (la compuerta de escritura de la CPU), `vm/execution_engine.h`, `driver/profiles.{h,cpp}`,
  `driver/src/machine_runner.cpp`; tests `devices/tests/test_sprites.cpp` y el de la compuerta en `vm/tests`.
- **Pasos**: OAM de 128 sprites en VRAM; tamaños de 8×8 a 64×64; flip; prioridad frente a las capas; límite de
  sprites por línea del perfil y bit de desbordamiento; en `micro` y `pocket` la CPU sólo escribe la VRAM en el
  VBlank o con la pantalla apagada.
- **Commit**: `Add hardware sprites (F8.3)`

### F8.4 · Tabla de líneas y efectos raster
- **Archivos**: `video/gpu.{h,cpp}` (recorrido por líneas: los tramos de estado de cada fotograma), `video/retro2d`
  (tabla de líneas), `video/gpu_executor.h` y `software_executor.cpp` (componer por tramos),
  `driver/src/machine_runner.cpp` (la ventana y `--frames` usan el fotograma recorrido); test
  `devices/tests/test_line_table.cpp`.
- **Pasos**: tabla en VRAM de (línea, registro, valor) que el scanout aplica al llegar a cada línea; la IRQ de línea
  sigue disponible; el `SoftwareExecutor` compone por línea (SPEC §7.6) para que ambos efectos sean exactos.
- **Commit**: `Add the line table for raster effects (F8.4)`

### F8.5 · STDLIB, herramienta y ejemplos
- **Repos**: STDLIB
- **Pasos**: `ceres/tiles.h` y `ceres/sprite.h` por hardware (los sprites por software pasan a `gfx.h`); herramienta
  del host `tools/img2tiles.js` (PNG → tileset, mapa y paleta, en C y CASM); ejemplos de plataformas y de
  laberinto con los perfiles `micro` y `retro`, con `.expected` por hash de fotogramas.
- **Commit**: `Add tile and sprite headers, img2tiles and retro examples (F8.5)`

### F8.6 · Documentación
- **Repos**: CeresASM, STDLIB
- **Archivos**: CeresASM `docs/31-Video.md` (V2, el recorrido por líneas, la VRAM en el VBlank), `docs/07`, `docs/30`,
  `docs/README.md`; STDLIB `docs/reference` (con `tools/gendocs.js`) y `README.md`.
- **Commit** (uno por repo): `Document the Retro 2D level (F8.6)`

## Cierre de la fase

- [x] Suites en verde: CeresASM `ctest` 11/11, Ceres-C `ctest` 11/11, STDLIB 319 comprobaciones con los dos runners.
- [x] Revisión `ocr` de cierre (delegada) sobre F8.5–F8.6.

## Notas

- **F8.1**: los registros de V2 viven en `video/retro2d.{h,cpp}` (un tipo valor, como pide §7.6 para copiarlo por
  tramos en F8.4) y la línea de V2 en software en `video/retro_scanout.{h,cpp}`, que el `SoftwareExecutor` llama por
  cada línea tras el plano bitmap. Las paletas se leen de la VRAM una vez por fotograma mientras su base no cambie.
  El mapa mide en píxeles una potencia de dos, así que el scroll (con el de la línea sumado, con signo) da la vuelta
  con una máscara. `Caps` pasa a `0xFF07` (V0–V2). `test_tile_layers` compara 16 combinaciones (tile 8 y 16, 4 y 8
  bpp, mapas de 32 a 128 por lado, scrolls grandes y negativos, tabla de scroll por línea) píxel a píxel con un
  modelo escrito en el test y fija su hash; aparte, prioridades (número de capa y bit del tile) y transparencia sobre
  el plano bitmap.
- **F8.2**: la capa afín usa las mismas entradas y tiles que las otras (volteos, banco de paleta y bit de
  prioridad incluidos); su Control es el de una capa con b11 (repetir). Las coordenadas se calculan en 64 bits y se
  desplazan con signo, así que −0,5 es el píxel −1 y ni los extremos (origen ±2^23, matriz ±128) desbordan.
  `test_affine_layer` compara con un modelo diez casos (identidad, desplazada, girada 30° en los dos sentidos,
  ampliada, reducida, espejada, cizallada, extremos; con vuelta y con recorte) y fija su hash.
- **F8.3**: `video/sprites.{h,cpp}` lee la OAM (sólo las entradas visibles, en orden) y elige los sprites de una
  línea; el scanout y `SpriteStatus` cuentan con lo mismo. Un sprite cuenta para el límite si toca la línea aunque
  esté fuera de la pantalla en horizontal (como en las consolas: se evalúa por Y). `SpriteStatus` se calcula en cada
  VBlank con una tabla de diferencias (una pasada por los sprites y otra por las líneas). La compuerta de VRAM es
  `vm::VramWriteGate` (en `vram.h`): el motor la consulta en los almacenamientos de la CPU en VRAM y en los trozos
  de las instrucciones de bloque (un trozo rechazado va a una página que nadie lee, así que la instrucción termina);
  sin compuerta, la ruta cuesta un puntero nulo. La GPU es la compuerta cuando `MachineProfile::vramInVblankOnly`
  (`micro` y `pocket`, y un `custom` hecho desde ellos). La DMA, el motor de copia y el terminal escriben siempre.
- **F8.4**: la GPU no dibuja mientras recorre: guarda los tramos de líneas que comparten registros
  (`video::LineState`: nivel, pantalla, color de fondo, plano bitmap y V2; el plano de texto no, se compone como
  esté) y compone en el VBlank. Antes de cualquier lectura o escritura de un registro, el recorrido alcanza el ciclo
  de la máquina aplicando la tabla de líneas línea a línea; tras una escritura, si cambió algo que el scanout lee,
  empieza un tramo en la siguiente línea por recorrer. `SpriteStatus` se cuenta por tramos. `compose()` sigue siendo
  «la pantalla con los registros de ahora» (tests, pantalla final); `composeScanned()` es el último fotograma
  recorrido, el que usan la ventana y `--frames`. Con eso un `Present` se ve un fotograma más tarde que en F5 (como
  en el hardware): `--frames` escribe el PNG en el VBlank siguiente al que aplicó el `Present` (y, si el programa
  termina antes, la pantalla de ese momento), así que sigue habiendo un PNG por `Present`. La tabla se limita a 8192
  entradas (SPEC §7.5) para que una tabla hostil no bloquee el host. Tropiezo: un registro que falte en la
  `RegisterMap` de la GPU no llega nunca al dispositivo (D19 lo descarta en el bus); hay un test que recorre todos
  los de V2.
- **Revisión `ocr` F8.1–F8.4** (modo delegado: el diff revisado a mano con las reglas de `ocr delegate rule`): un
  hallazgo real, corregido. La tabla de líneas se leía de la VRAM al alcanzar el recorrido cada línea, y el
  recorrido va con retraso (en el siguiente acceso a un registro o en el VBlank). Así, un cambio en la tabla hecho a
  mitad de fotograma podía valer para líneas ya pasadas, y una lectura del depurador cambiaba cuándo se leía. Ahora
  la GPU la lee entera en el primer ciclo de la línea 0 de cada fotograma (evento `FrameEvent`, 60 por segundo) y
  lo cambiado después vale para el siguiente (SPEC §7.5); con eso ningún acceso del depurador altera nada. Se
  descartó otro: la VRAM puede quedarse apuntando a la GPU como compuerta si ésta se destruye sin desconectarse,
  pero el runner la desconecta siempre y en la sesión del depurador la máquina ya no corre cuando la GPU se
  destruye (las escrituras de memoria del depurador van sólo a la RAM).
- **F8.5** (lib@34b0ccb): `ceres/tiles.h` (capas, capa afín, tabla de líneas; `LINE_ENTRY`), `ceres/sprite.h` pasa a
  ser la OAM por hardware (una copia en RAM que `sprites_commit()` lleva a la VRAM con el motor de copia, que en
  `micro` y `pocket` puede escribir en cualquier momento) y los sprites por software pasan a `gfx.h`
  (`src/ceres/gfx_sprite.c`). `video.h` gana `video_vram_alloc` (bloques de 256 tras el scrollback, la misma cuenta
  que hace la GPU al arrancar), `video_vram_reset` y `video_vram_resets`; `fb.h` toma de ahí sus búferes y ya no
  baja de V2 a V1. `tools/img2tiles.js` (Node sin dependencias: decodificador PNG propio con los cinco filtros, 1–8
  bits, paleta con `tRNS`) reparte los colores en bancos de 15, deduplica tiles también volteados y en otro banco,
  y rellena el mapa a 32/64/128; en CASM cada array va en una línea (el ensamblador no admite un literal repartido).
  Ejemplos: `maze.c` en `micro` (escribe los puntos del camino con la CPU justo tras `video_wait_vblank()`, y dice
  al final si se perdió alguna escritura: ninguna) y `platformer.c` en `retro` (parallax, degradado con la tabla de
  líneas, héroe y monedas como sprites). Con `printf` el laberinto no cabía en los 64 KiB de `micro` (el formateo
  de `double` son unos 35 KB): escribe sus números a mano con `puts`, y se enlaza con `--gc-sections` (en su
  `.flags`). Los runners aceptan en los ejemplos `.run` (el perfil) y `.frames` (SHA-256 de cada PNG de
  `--frames`), y comprueban `tools/img2tiles.cases` (tests de la herramienta y cabeceras de `examples/art`, cuyos
  PNG dibuja `examples/art/make_art.js`). Revisión de cierre (delegada): un hallazgo real, corregido en
  lib@1b1d813 (una línea de `img2tiles.cases` sin argumentos hacía contar `1..0` hacia atrás en PowerShell).
- **F8.6** (lib@f401f68 y el commit de CeresASM que añade esta nota): `docs/31-Video.md` describe V2 entero, el
  recorrido por líneas y la regla de la VRAM de `micro` y `pocket`; la referencia de la STDLIB sale de las
  cabeceras. Para F10: V3 tiene libres `0x324–0x33F` y `0x3E0–0x3FF` del slot `0x40`; las capas 4–7 y los 1024
  sprites afines necesitarán más (el resto del slot `0x40` pasado `0x3FF`, o el `0x42`), y las palabras 2 y 3 de
  cada entrada de OAM están reservadas para V3.

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
- **Commit**: `Document the Retro 2D level (F8.6)`

## Notas

- **F8.1**: los registros de V2 viven en `video/retro2d.{h,cpp}` (un tipo valor, como pide §7.6 para copiarlo por
  tramos en F8.4) y la línea de V2 en software en `video/retro_scanout.{h,cpp}`, que el `SoftwareExecutor` llama por
  cada línea tras el plano bitmap. Las paletas se leen de la VRAM una vez por fotograma mientras su base no cambie.
  El mapa mide en píxeles una potencia de dos, así que el scroll (con el de la línea sumado, con signo) da la vuelta
  con una máscara. `Caps` pasa a `0xFF07` (V0–V2). `test_tile_layers` compara 16 combinaciones (tile 8 y 16, 4 y 8
  bpp, mapas de 32 a 128 por lado, scrolls grandes y negativos, tabla de scroll por línea) píxel a píxel con un
  modelo escrito en el test y fija su hash; aparte, prioridades (número de capa y bit del tile) y transparencia sobre
  el plano bitmap.

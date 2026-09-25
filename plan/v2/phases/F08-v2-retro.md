# F8 · V2: Retro 2D

- **Tamaño**: L · **Depende de**: F5, decisión P04 · **Repos**: CeresASM, STDLIB
- **Objetivo**: tiles, sprites y efectos raster con color indexado, programados por registros y tablas en VRAM,
  como las consolas de 8 y 16 bits. Todo en el `SoftwareExecutor` (el hardware llega en F12). P04 sólo bloquea F8.3.
- **SPEC**: §7.1 (V2), §7.2, §7.3 (`0x300–0x3FF`, que esta fase fija y pasa a NORMATIVA).

## Antes de empezar

Fija en SPEC §7.3 la disposición exacta de `0x300–0x3FF` (capas, capa afín, sprites, tabla de líneas) y el
formato de las entradas en VRAM (entrada de mapa de tiles de 16 bits: índice, paleta, flip, prioridad; entrada
de OAM de 16 bytes: x, y, tile, tamaño, paleta, flip, prioridad). Commit `plan: fix the V2 register layout`.

## Tareas

### F8.1 · Paletas y capas de tiles
- **Depende de**: F5 y el commit de «Antes de empezar»
- **Pasos**: 16 paletas de 16 y una de 256 en VRAM; 4 capas (mapa, tileset, tamaño de mapa 32–128, tile 8×8 o
  16×16, 4 u 8 bpp, scroll, prioridad, banco de paleta, tabla de scroll por línea).
- **Aceptación**: [ ] Tests por hash con cada combinación de tamaño y bpp.
- **Commit**: `Add palettes and tile layers (F8.1)`

### F8.2 · Capa afín
- **Pasos**: una capa con matriz 2×2 en 8.8 y origen; modo de vuelta o de recorte.
- **Commit**: `Add the affine layer (F8.2)`

### F8.3 · Sprites · requiere P04
- **Pasos**: OAM de 128 sprites en VRAM; tamaños de 8×8 a 64×64; flip; prioridad frente a las capas; límite de
  sprites por línea del perfil y bit de desbordamiento; en `micro` y `pocket`, si P04 lo decide, la VRAM sólo se
  escribe durante el VBlank.
- **Commit**: `Add hardware sprites (F8.3)`

### F8.4 · Tabla de líneas y efectos raster
- **Pasos**: tabla en VRAM de (línea, registro, valor) que el scanout aplica al llegar a cada línea; la IRQ de línea
  sigue disponible; el `SoftwareExecutor` compone por línea para que ambos efectos sean exactos.
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

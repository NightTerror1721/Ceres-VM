# F13 · V4: Vectorial 2D

- **Tamaño**: XL · **Depende de**: F12 · **Repos**: CeresASM, STDLIB
- **Objetivo**: 2D de resolución independiente (trazados con antialiasing, degradados, transformaciones, recortes,
  texto escalable) en software y hardware. **Hito 7.**
- **SPEC**: §7.1 (V4); fija la parte vectorial del slot `0x42` y los paquetes vectoriales.

## Tareas

### F13.1 · Trazados y comandos
- **Pasos**: formato de trazado en VRAM (MOVE, LINE, QUAD, CUBIC, ARC, CLOSE en punto fijo o float); paquetes
  FILL_PATH, STROKE_PATH, SET_PAINT, PUSH/POP_TRANSFORM, PUSH/POP_CLIP, BEGIN/END_GROUP.
- **Commit**: `Add vector paths and their commands (F13.1)`

### F13.2 · Rasterizador de cobertura en software
- **Pasos**: aplanado de curvas con tolerancia fija; cobertura por scanline con acumulación de área firmada;
  relleno nonzero y evenodd; trazo (grosor, uniones, extremos, discontinuos) convertido a relleno.
- **Aceptación**: [ ] Tests por hash de formas de referencia.
- **Commit**: `Rasterize vector paths in software (F13.2)`

### F13.3 · Pintura, transformaciones, recortes y grupos
- **Pasos**: color, degradados lineales, radiales y cónicos, patrones de imagen; pila de transformaciones 3×2;
  recortes por trazado; grupos con opacidad y modos Porter-Duff.
- **Commit**: `Add paints, transforms, clips and groups (F13.3)`

### F13.4 · Texto por contornos
- **Pasos**: herramienta `tools/font2path.js` (TTF → contornos en un formato propio); dibujo de texto por
  contornos con caché de glifos.
- **Commit**: `Draw text from font outlines (F13.4)`

### F13.5 · V4 en hardware
- **Pasos**: teselado a triángulos y stencil-then-cover o MSAA en `SDL_GPU`; tolerancia de antialiasing documentada.
- **Commit**: `Draw vector paths on the GPU (F13.5)`

### F13.6 · STDLIB y documentación
- **Pasos**: `ceres/canvas.h`; ejemplo de interfaz escalable; `docs/31-Video.md` (V4).
- **Commit**: `Add canvas.h and document V4 (F13.6)`

## Notas

# F10 · V3: Arcade 2D

- **Tamaño**: XL · **Depende de**: F8 · **Repos**: CeresASM, STDLIB
- **Objetivo**: 2D de color verdadero con alfa y transformaciones, y el procesador de comandos que alimenta al
  motor de dibujo 2D. Absorbe al blitter. Con F11 forma el **hito 5**.
- **SPEC**: §7.1 (V3); fija y pasa a NORMATIVA el slot `0x41` (procesador de comandos) y la parte 2D del `0x42`.

## Antes de empezar

Fija en SPEC el formato de paquete (`op:8 | flags:8 | len:16`), la lista de paquetes (control: NOP, JUMP, CALL,
RET, FENCE, WAIT_VBLANK, SET_REG; superficies y estado; 2D: FILL, COPY, BLIT, LINES, TRIANGLES; coherencia:
FLUSH_RANGE, INVALIDATE_RANGE), el modelo de coste en ciclos de GPU y los códigos de `FaultCode`.
Commit `plan: fix the command processor and 2D engine`.

## Tareas

### F10.1 · Procesador de comandos
- **Pasos**: anillo en RAM o VRAM (base, tamaño, head, tail como doorbell); decodificación y validación de
  paquetes; FENCE con IRQ 34; fallos con `FaultCode`/`FaultAddress` e IRQ 35; watchdog contra bucles de JUMP;
  el coste modelado decide cuándo se publican `Busy`, fences e IRQ.
- **Aceptación**: [ ] Fuzzing: paquetes aleatorios nunca rompen el host, sólo dan fallos de la máquina.
- **Commit**: `Add the GPU command processor (F10.1)`

### F10.2 · Motor 2D
- **Pasos**: slots de superficie; destino y clip; FILL; COPY; BLIT con clave, alfa, flip, escala y rotación en
  16.16; LINES; TRIANGLES planos, Gouraud y texturizados; mezclas (none, alpha, add, sub, mul, screen);
  conversión de formatos; render a textura. Regla top-left y punto fijo con 4 bits de subpíxel.
- **Aceptación**: [ ] Tests por hash de cada operación y mezcla.
- **Commit**: `Add the 2D drawing engine (F10.2)`

### F10.3 · Capas y sprites de V3
- **Pasos**: hasta 8 capas en color verdadero o indexado; 1024 sprites con matriz afín propia; alfa por píxel;
  ventanas y máscaras por capa; mosaico; filtrado bilineal; límites por línea del perfil.
- **Commit**: `Add true-colour layers and affine sprites (F10.3)`

### F10.4 · Retirar el blitter y STDLIB
- **Repos**: CeresASM, STDLIB
- **Pasos**: borrar `video/blitter.{h,cpp}` y liberar su slot temporal; STDLIB: `gpu.h` (command buffers, fences),
  `gpu2d.h`; `gfx.h` usa el motor 2D si hay V3; borrar `blitter.h`.
- **Commit** (uno por repo): `Replace the blitter with the 2D engine (F10.4)`

### F10.5 · Ejemplos y documentación
- **Pasos**: shmup con cientos de sprites en el perfil `arcade`; `docs/31-Video.md` (V3).
- **Commit**: `Add arcade examples and document V3 (F10.5)`

## Notas

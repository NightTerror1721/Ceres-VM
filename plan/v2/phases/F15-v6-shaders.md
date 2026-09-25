# F15 · V6: 3D programable (opcional)

- **Tamaño**: XL · **Depende de**: F14, decisiones P15 (que se haga) y P16 · **Repos**: CeresASM (Ceres-C si P16 elige C)
- **Objetivo**: vertex, pixel y compute shaders escritos por el programa.
- **SPEC**: §7.1 (V6); crea la sección de CSIR.

## Tareas

### F15.0 · Lenguaje de shaders · `HUMANO`
- **Decisiones**: P16. **Commit**: `Close the shader language decision (F15.0)`

### F15.1 · CSIR y su ensamblador
- **Pasos**: especificación (registros vec4 temporales, entradas, salidas, constantes; unas 40 operaciones;
  bucles acotados; límite de instrucciones; coste en ciclos de GPU); `ceres shader` produce el bytecode;
  validación estática.
- **Commit**: `Define CSIR and assemble shaders (F15.1)`

### F15.2 · Intérprete en software
- **Commit**: `Interpret shaders in software (F15.2)`

### F15.3 · CSIR a SPIR-V y hardware
- **Pasos**: emisor de SPIR-V; en D3D12 y Metal, SDL_shadercross en ejecución o sólo Vulkan (según P15).
- **Commit**: `Translate shaders to SPIR-V for the GPU (F15.3)`

### F15.4 · Compute
- **Pasos**: paquete DISPATCH y búferes de lectura/escritura en VRAM.
- **Commit**: `Add compute shaders (F15.4)`

### F15.5 · Documentación y ejemplos
- **Commit**: `Document V6 and add shader examples (F15.5)`

## Notas

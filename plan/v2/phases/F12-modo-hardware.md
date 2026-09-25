# F12 · Modo hardware para V0–V3

- **Tamaño**: XL · **Depende de**: F10, decisión P15, spike F0.3 · **Repos**: CeresASM
- **Objetivo**: el mismo programa, sin cambios, sobre la GPU real del host con `SDL_GPU`, indistinguible del modo
  software salvo en la velocidad. **Hito 6.**
- **SPEC**: §1 (invariante 6), §7, §10 (`--gpu`).

## Tareas

### F12.0 · Alcance y orden · `HUMANO`
- **Decisiones**: P15. **Commit**: `Close the scope decision (F12.0)`

### F12.1 · `SdlGpuExecutor` y selección de ejecutor
- **Archivos**: `libs/sdl/src/sdl_gpu_executor.{h,cpp}`; `GpuExecutor` en `libs/devices/.../video/`.
- **Pasos**: dispositivo `SDL_GPU` y swapchain de la ventana; `--gpu auto|software|hardware` (sin ventana,
  siempre software); bit 31 de `Caps`; si falla la creación o se pierde el dispositivo, reconstruir desde la VRAM
  o caer a software sin perder estado.
- **Commit**: `Add the SDL_GPU executor and executor selection (F12.1)`

### F12.2 · Coherencia de VRAM y caché de recursos
- **Pasos**: el bitmap de páginas de `Vram` (F4.2) marca lo escrito por la CPU; caché de texturas y búferes por
  (dirección, formato, tamaño, pitch) invalidada por página; subidas por lotes en copy passes; los render targets
  son de la GPU y se descargan de forma perezosa si la CPU lee sus páginas; `FLUSH_RANGE`/`INVALIDATE_RANGE` como pistas.
- **Commit**: `Keep VRAM coherent with the host GPU (F12.2)`

### F12.3 · Shaders precompilados y compositor
- **Archivos**: `libs/sdl/shaders/` (fuentes y blobs SPIR-V, DXIL, MSL incrustados); objetivo CMake opcional para regenerarlos.
- **Pasos**: shader de composición para texto, bitmap, capas y sprites de V2–V3, con la tabla de líneas como textura.
- **Commit**: `Compose V0 to V3 on the GPU (F12.3)`

### F12.4 · Motor 2D en la GPU
- **Pasos**: pipelines para FILL, COPY, BLIT (clave, alfa, escala, rotación), líneas y triángulos, mezclas y clip.
- **Commit**: `Run the 2D engine on the GPU (F12.4)`

### F12.5 · Conformidad y métricas
- **Pasos**: suite opt-in (`CERES_GPU_TESTS=hw`) que ejecuta escenas en los dos ejecutores y compara con la
  tolerancia documentada; métricas de bytes subidos y descargados por fotograma en `ceres profile`; en CI, lavapipe
  o WARP si F0.3 lo confirmó.
- **Commit**: `Add the hardware conformance suite (F12.5)`

### F12.6 · Documentación
- **Commit**: `Document the hardware executor (F12.6)`

## Notas

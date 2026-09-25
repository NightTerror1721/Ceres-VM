# Spike de SDL_GPU (F0.3)

Prototipo aparte, fuera de `Ceres/` y del build normal, para comprobar que `SDL_GPU` (SDL 3.4.16) da lo que
necesita el modo hardware de la GPU de la Máquina Ceres v2 (fase F12): subir una imagen de VRAM, dibujarla en un
quad, presentarla y leerla de vuelta para compararla con el ejecutor software.

## Qué hace

`sdl_gpu_spike [--driver <nombre>] [--size <an>x<al>] [--frames <n>] [--offscreen] [--immediate]`

1. Lista los drivers GPU con los que se compiló SDL e intenta crear un dispositivo con cada uno.
2. Crea un dispositivo, sube con un *copy pass* un patrón de `an×al` píxeles en `B8G8R8A8_UNORM` (el orden de bytes
   de un píxel `0x00RRGGBB` de Ceres en memoria), lo dibuja con un quad texturizado (muestreo *nearest*) en un
   destino fuera de pantalla del mismo tamaño, lo descarga con `SDL_DownloadFromGPUTexture` y lo compara píxel a
   píxel con el patrón.
3. Mide el bucle fuera de pantalla (subir + dibujar + esperar a la GPU) y, salvo con `--offscreen`, abre una
   ventana, la reclama con `SDL_ClaimWindowForGPUDevice` y mide cada fotograma: rellenar, subir, dibujar en la
   *swapchain* y presentar. Por defecto con vsync; `--immediate` usa `SDL_GPU_PRESENTMODE_IMMEDIATE` si la
   ventana lo admite.

Escribe una línea `SPIKE …` por resultado y termina con código 3 si la lectura no coincide.

## Compilar

```bash
cmake -S spikes/sdl_gpu -B spikes/sdl_gpu/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DFETCHCONTENT_SOURCE_DIR_SDL3=D:/Projects/CeresASM/Ceres/build/gcc-sdl-ipo/_deps/sdl3-src
cmake --build spikes/sdl_gpu/build
spikes/sdl_gpu/build/sdl_gpu_spike.exe
```

`FETCHCONTENT_SOURCE_DIR_SDL3` reutiliza el SDL que ya descargó el preset `gcc-sdl-ipo`; sin él, CMake descarga
`release-3.4.16`. `build/` está en `.gitignore`.

## Resultados en la máquina de desarrollo

Ryzen 7 9800X3D, NVIDIA GeForce RTX 5070 Ti (driver 32.0.16.1088) más la gráfica integrada AMD Radeon, monitor a
144 Hz, Windows 11 Pro 26200, GCC 15.2 (MSYS2).

**Backends.**

| Driver SDL | ¿Arranca? | Formatos de shader que acepta | Probado de punta a punta |
| --- | --- | --- | --- |
| `direct3d12` | sí | DXBC, DXIL | **sí**: lectura exacta a 320×240, 1280×720 y 1920×1080 |
| `vulkan` | sí | SPIR-V | no: falta un compilador de SPIR-V (ver «Cadena de shaders») |
| `metal` | no existe en Windows | MSL, metallib | no |

**Lectura de vuelta.** `SDL_DownloadFromGPUTexture` devuelve la imagen dibujada **idéntica bit a bit** al patrón
subido (0 píxeles distintos en los tres tamaños). Con muestreo *nearest* y un quad que cubre el destino sin
escalar, la GPU no altera ningún valor: eso permite los tests de comparación entre el modo hardware y el software
del plan (F12). Con escalado, filtrado o mezcla, la exactitud entre GPUs no está garantizada, y esos tests deberán
comparar sólo lo que el plan declara exacto.

**Coste por fotograma** (mediana de una ejecución de 300–600 fotogramas, CPU del hilo que envía):

| Tamaño | Fuera de pantalla: subir + dibujar + esperar | Ventana, CPU en SDL_GPU (subir + dibujar + enviar) | Ventana, fps con `--immediate` | SDL_Renderer actual (F0.2) |
| --- | ---: | ---: | ---: | ---: |
| 320×240 | 90 µs | 166 µs (296 con vsync) | 1 900 | 96 µs |
| 1280×720 | 238 µs | 341 µs (434 con vsync) | 585 | 590 µs |
| 1920×1080 | 441–460 µs | 522 µs | 311 | — |

- Con vsync presenta a 140 fps: la frecuencia del monitor.
- El coste es sobre todo fijo (unos 150–250 µs por envío). A 320×240 es más caro que el `SDL_Renderer` de hoy; a
  1280×720 cuesta un 40 % menos. Presentando una vez por vblank (60 Hz), 1920×1080 sale por unos 31 ms de CPU por
  segundo: un 3 % de un núcleo.
- Generar la imagen en la CPU (lo que hará el ejecutor software o el programa) cuesta más que enviarla: el relleno
  de prueba lleva 1,2 ms a 1280×720 y 2,6 ms a 1920×1080. Subir sólo las páginas de VRAM que han cambiado (plan,
  F12) importa más que el backend.

## CI: WARP y lavapipe

- **Elegir backend**: la pista `SDL_HINT_GPU_DRIVER` (variable de entorno `SDL_GPU_DRIVER=direct3d12|vulkan`) o el
  nombre en `SDL_CreateGPUDevice`.
- **WARP (D3D12 por software)**: SDL 3.4.16 **no permite pedirlo**. `SDL_gpu_d3d12.c` toma el adaptador 0 de
  `EnumAdapterByGpuPreference` (alto rendimiento, o bajo consumo con `SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN`).
  En una máquina sin GPU (un runner de CI de Windows sin gráfica) el adaptador 0 es «Microsoft Basic Render Driver»,
  que es WARP, y funciona sin hacer nada. En una máquina con GPU no hay forma de forzarlo sin parchear SDL.
- **lavapipe (Vulkan por software)**: SDL acepta dispositivos de CPU salvo que se pida
  `SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN`, pero prefiere los discretos e integrados.
  Para forzarlo hay que dejar al cargador de Vulkan sólo su ICD (`VK_DRIVER_FILES`, antes `VK_ICD_FILENAMES`).
  En Linux (mesa-vulkan-drivers) es lo habitual para CI. No se ha probado aquí: no hay lavapipe instalado.

## Cadena de shaders

- **Fuente**: un único HLSL ([quad.hlsl](quad.hlsl)). Sigue la disposición de recursos de `SDL_CreateGPUShader`:
  las texturas muestreadas del fragment shader van en el espacio de registros 2 (el *descriptor set* 2 en SPIR-V),
  y en SPIR-V textura y sampler van juntos como *combined image sampler*
  (`[[vk::combinedImageSampler]]`, que sólo se aplica cuando dxc genera SPIR-V).
- **DXIL (D3D12)**: el `dxc.exe` del Windows SDK 10.0.26100 (DXC 1.8.2502) lo compila y lo firma con el `dxil.dll`
  que lleva al lado. Funciona tal cual.
- **SPIR-V (Vulkan)**: **el `dxc` del Windows SDK no puede**. Anuncia `-spirv`, pero falla con «SPIR-V CodeGen not
  available». Hace falta el `dxc` del Vulkan SDK o el de una release de DirectXShaderCompiler (o glslang con una
  fuente GLSL). El `CMakeLists.txt` lo comprueba al configurar y, si no puede, compila el spike sólo para D3D12.
- **MSL (Metal)**: SDL_shadercross (convierte SPIR-V o HLSL a MSL, al compilar o en tiempo de ejecución) o
  spirv-cross.
- **Incrustar**: [embed.cmake](embed.cmake) convierte cada blob en un array de C al compilar, y el programa lo pasa
  a `SDL_CreateGPUShader`. SDL_shadercross en tiempo de ejecución evitaría los blobs a cambio de una dependencia más.

**Recomendación para F12.** El modo hardware debería llevar los shaders precompilados (DXIL, SPIR-V y MSL)
**incluidos en el repositorio**, con un script que los regenere cuando cambie el HLSL. Así, compilar Ceres sigue sin
pedir nada más que un compilador de C++23 y la descarga de SDL (el principio de `cmake/Sdl3.cmake`), y el
compilador de shaders sólo lo necesita quien cambie un shader. Antes de F12 hay que instalar un dxc con SPIR-V para
probar el backend Vulkan.

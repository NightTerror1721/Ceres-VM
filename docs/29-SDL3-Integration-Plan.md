# Plan de integración de SDL3

[← Back to index](README.md)

> **Estado:** propuesta de diseño, aún sin implementar. Este documento presenta el plan y **dos
> propuestas alternativas** (A y B) más un paso 0 recomendado.

## Decisión (consensuada)

| Decisión | Elección |
| --- | --- |
| Arquitectura | **Propuesta A** — SDL tras una interfaz `HostBackend`, en una biblioteca nueva `libs/sdl`; SDL no toca `core`/`vm`/`devices`/`asm`. |
| Código de tecla | **Ensanchar a 32 bits** — `KeyboardDevice` pasa a eventos de 32 bits (código en 30 bits + flag pressed/released), fiel a los scancodes de SDL. |
| Framebuffer | **Píxeles desde el inicio** — modo píxel RGB32 + textura SDL en la primera integración (las Fases 1 y 2 del plan se funden). |

Las propuestas A y B siguen descritas abajo por referencia; el plan de fases de §8 refleja ya la
elección A.

## Progreso

| Fase | Estado |
| --- | --- |
| 0 — Spike | Hecho (`8c7c06c`) — bucle cooperativo y mapeo de entrada validados. |
| 1 — Dispositivos | Hecho (`c49a949`) — teclado a 32 bits y `DisplayDevice` (píxeles RGB32, slot 7). |
| 2 — SDL backend | Hecho — `HostBackend`/`HeadlessBackend`, `libs/sdl` (`SdlBackend`), `ceres run --window`. |
| 3 — Gamepad | Hecho — `GamepadDevice` (slot 8, `UserInterrupt5`) + mapeo `SDL_Gamepad`. |
| 4 — Debugger + docs | Hecho — el debugger adjunta todos los dispositivos; SDL queda solo en `run` (ver §7). |

## 1. Objetivo

Integrar [SDL3](https://libsdl.org/) como capa de presentación y de entrada real para Ceres, de modo
que el mismo `ceres run` (y, más adelante, `ceres debug`) pueda:

- mostrar el framebuffer en una ventana real (hoy solo hay una rejilla de texto);
- leer teclado y ratón **de verdad** (hoy solo `stdin`, line-buffered, que va al terminal — no al
  `KeyboardDevice`/`MouseDevice`, que quedan vacíos);
- preparar el terreno para un futuro gamepad (roadmap, Fase 3/5).

Sin SDL, Ceres debe seguir compilando y pasando los tests exactamente igual que hoy.

## 2. Estado actual (lo que ya existe y no cambia)

- **Dispositivos** (`libs/devices`, header-only): `TerminalDevice`, `FramebufferDevice` (rejilla de
  texto, `setPresentSink(std::string_view)`), `KeyboardDevice::pushKey(u8 code, bool pressed)`,
  `MouseDevice::pushMotion(dx, dy, buttons, wheel)`. Todos se alimentan **desde el host** vía C++.
- **Driver** (`libs/driver`): `Machine` (envuelve VM + dispositivos, expone `pushInput/pushKey/
  pushMouse`) y `runMachine()` (lo que usa `ceres run`/`ceres profile`). El bucle de ejecución es
  `CeresVM::run()` → `while (isPoweredOn()) engine.step();`.
- **CLI** (`apps/cli/src/main.cpp`): un `main` de 6 líneas que delega en `runCommandLine` con
  `std::cin/cout/cerr`. El único input real es `stdin → terminal.pushInput(c)`.
- **Debugger** (`libs/debug`): `DebugSession` + `DebugCLI` (REPL por stdin) + `DebugServer` (NDJSON).
- **Build**: CMake, `ceres_add_library()`, C++23, presets `msvc`/`ninja`/`gcc`/`clang`. **Cero
  dependencias externas** (ni FetchContent ni find_package ni vcpkg).

Interrupciones libres para el gamepad: `UserInterrupt5` (21). Slot MMIO libre: `8`.

## 3. Decisiones transversales (independientes de A/B)

1. **Opcionalidad.** SDL3 debe ser una dependencia **opcional**, apagada por defecto, para no romper
   la matriz de 4 compiladores del CI. Nuevo `option(CERES_ENABLE_SDL ... OFF)`; el CLI gana un flag
   (`ceres run --window`, nombre a decidir) que solo existe/actúa si se compiló con SDL.
2. **Cómo se obtiene SDL3.** Dos vías, combinables:
   - `FetchContent` con un tag fijado (`release-3.2.x`) — autocontenido, ideal para CI.
   - `find_package(SDL3)` como *override* si el usuario ya lo tiene instalado (dev en el sistema,
     vcpkg, etc.). `FetchContent` es la opción por defecto recomendada.
3. **Hilos / bucle de eventos.** SDL exige que la ventana y `SDL_PollEvent()` vivan en el hilo que
   llamó a `SDL_Init` (el principal, sobre todo en macOS). El bucle `CeresVM::run()` actual no bombea
   eventos. La solución simple y recomendada es un **bucle cooperativo de un solo hilo**: ejecutar N
   instrucciones, bombear `SDL_PollEvent()`, presentar el frame, repetir. (La alternativa —VM en hilo
   aparte con cola de eventos— es más compleja y no se justifica en la fase inicial.)
4. **Mapeo de teclado.** SDL distingue `scancode` (físico, independiente del layout) de `keycode`
   (lógico, dependiente del layout). Para una VM lo natural es el **scancode físico**. Pero hoy
   `KeyboardDevice` guarda el código en un `u8` (256 valores) y los scancodes de SDL llegan a ~300.
   Hay que **ensanchar el código de tecla** (evento de 32 bits: código en bits 30:0 + flag
   pressed/released) o definir una tabla compacta "Ceres scancode". Ver §7.
5. **Ratón.** SDL da `MouseMotionEvent` (relativo `xrel/yrel` + absoluto `x/y`) y
   `MouseButtonEvent`/`MouseWheelEvent`. Traducción directa a `MouseDevice`: motion → `pushMotion(xrel,
   yrel, máscaraBotones, 0)`, botón → actualizar máscara y `pushMotion(0,0,máscara)`, rueda →
   `pushMotion(0,0,máscara,delta)`. El modo ratón relativo (`SDL_SetRelativeMouseMode`) encaja con los
   deltas; el absoluto, con `XRegister`/`YRegister`.
6. **Framebuffer.** Dos niveles, en fases distintas (ver §6): (fase 1) renderizar la rejilla de texto
   actual con una fuente (SDL_ttf o una fuente bitmap embebida); (fase 2) framebuffer **de píxeles**
   (RGB32) bliteado como textura SDL — esto es lo que convierte a Ceres en la "consola retro" del
   roadmap. El modo píxel puede ser un modo nuevo de `FramebufferDevice` o un `DisplayDevice` nuevo.

## 4. Propuesta A — backend desacoplado tras una interfaz (`HostBackend`)

SDL vive en una biblioteca nueva y **nada de SDL entra en `core`/`vm`/`devices`/`asm`**.

- **Nueva interfaz** en `libs/driver`: `HostBackend` (o `HostDisplay`) con `open(...)`, `poll()` (bombea
  eventos → `pushKey/pushMouse/pushInput`), `present(frame)` y `close()`.
- **Dos implementaciones:**
  - `HeadlessBackend`: el comportamiento actual (stdio; el framebuffer sigue saliendo por stdout).
  - `SdlBackend`: en una **biblioteca nueva** `libs/sdl` (o `libs/host_sdl`), compilada solo con
    `CERES_ENABLE_SDL`. Crea la ventana, bombea eventos, mapea scancodes/mouse y presenta el frame.
- `Machine` (o un `Runner` nuevo) elige el backend en **runtime** según el flag `--window`.
- El CLI (`main.cpp`) sigue siendo trivial; la selección es interna al driver.

**Ventajas:** separación limpia; el build headless (CI de 4 compiladores) no ve SDL en absoluto; el
backend headless sigue siendo trivial de testear; facilita añadir un futuro backend (p. ej. web/raylib).

**Inconvenientes:** una capa de abstracción más; más ficheros; el contrato de `HostBackend` hay que
diseñarlo bien desde el inicio (riesgo de rehacerlo al añadir gamepad/píxeles).

## 5. Propuesta B — SDL3 como dependencia opcional directa del driver + framebuffer a píxeles

Sin interfaz nueva: SDL3 es una dependencia opcional de `libs/driver` y `runMachine`/`Machine` ganan
directamente el camino de ventana.

- `runMachine` detecta `--window` y, en ese caso, inicializa SDL, crea la ventana y ejecuta el bucle
  cooperativo (step N + `SDL_PollEvent` + present) en lugar de `CeresVM::run()`.
- El framebuffer se extiende (o se añade `DisplayDevice`) a **modo píxel desde el inicio**: el programa
  escribe píxeles RGB32 en RAM y SDL los sube como textura; la rejilla de texto queda como modo
  compatible.
- El mapeo teclado/ratón vive en el mismo fichero que `runMachine`.

**Ventajas:** menos piezas, iteración más rápida, gráficos de píxeles desde el día uno.

**Inconvenientes:** `libs/driver` crece y mezcla presentación con lógica de comandos; los tipos de SDL
aparecen en cabeceras del driver (aunque se puede ocultar con un Pimpl/`Impl`); el build headless
sigue necesitando la opción para no enlazar SDL.

## 6. Paso 0 recomendado (spike, antes de decidir A/B)

Una prueba de concepto **sin tocar las bibliotecas**: un ejecutable/ejemplo mínimo que usa solo las
APIs públicas ya existentes para validar las dos incógnitas de mayor riesgo:

1. el **bucle cooperativo** (step + `SDL_PollEvent` + present) contra el `CeresVM`/`Machine` actual;
2. el **mapeo de scancodes** y del ratón reales (¿qué se pierde con `u8`? ¿hace falta `u32`?).

Es barato, despeja las dudas de §7 y hace que la elección A/B se base en hechos. Recomendado hacerlo
antes de comprometer la arquitectura.

## 7. Preguntas abiertas / riesgos

- ~~**Anchura del código de tecla**~~ → **decidido:** se ensancha `KeyboardDevice` a 32 bits (evento
  de 32 bits: código en bits 30:0 + flag pressed/released). Cambio de ABI del dispositivo → actualizar
  docs 07 y los tests de `input_devices.h`.
- **`ceres debug` + SDL** → **decidido:** SDL se queda solo en `ceres run --window`. El REPL del
  debugger es dueño de stdin y de cada paso de instrucción, dos cosas que una ventana SDL no
  comparte bien: no hay forma limpia de que el mismo terminal alimente a la vez al REPL y a la
  ventana, ni de encajar `SDL_PollEvent` en el bucle `stepOnce()`. El debugger **sí** adjunta el
  display, el teclado, el ratón, el gamepad y el DMA, de modo que un programa depurado ve la misma
  máquina que uno ejecutado y su estado se inspecciona igual (el display no presenta nada sin sink,
  pero su buffer de píxeles es visible por memoria). Un debugger con ventana sería un protocolo
  distinto, fuera del alcance de este plan.
- **Fuente para el modo texto:** aunque se va a píxeles desde el inicio, el modo texto de
  `FramebufferDevice` debe seguir funcionando en headless; si más adelante se renderiza en ventana,
  elegir entre SDL_ttf (dependencia extra) y fuente bitmap embebida.
- **Gamepad:** nuevo `GamepadDevice` (slot 8, `UserInterrupt5`) mapeando `SDL_Gamepad` → botones/ejes.
  Solo esbozar la interfaz ahora; implementarlo en una fase posterior.
- **Determinismo:** con SDL activo, la entrada deja de ser reproducible (igual que `stdin` hoy). No
  afecta a los tests, que inyectan eventos directamente en los dispositivos.

## 8. Plan de acción por fases (común a A y B, adaptado a A)

Cada fase es un commit (o varios) autosuficiente y con sus tests.

0. **Fase 0 — Spike** (opcional ahora, véase §6): validar el bucle cooperativo y el mapeo de entrada
   con un ejemplo mínimo que usa solo las APIs públicas. Sigue siendo útil para despejar el bucle
   de eventos antes de tocar las bibliotecas.
1. **Fase 1 — Dispositivos** (independiente de SDL): ensanchar `KeyboardDevice` a eventos de 32 bits
   (código + flag), y añadir el **modo píxel RGB32** a `FramebufferDevice` (o un `DisplayDevice`).
   Docs 07 + tests. Esto desacopla el trabajo de dispositivo del de SDL.
2. **Fase 2 — SDL backend**: `option(CERES_ENABLE_SDL)` + FetchContent; biblioteca `libs/sdl`;
   interfaz `HostBackend` en `libs/driver` con `HeadlessBackend` y `SdlBackend`; `ceres run --window`
   con bucle cooperativo (step N + `SDL_PollEvent` + present), teclado/ratón reales y framebuffer
   píxel bliteado como textura.
3. **Fase 3 — Gamepad:** `GamepadDevice` (slot 8, `UserInterrupt5`) + mapeo `SDL_Gamepad`.
4. **Fase 4 — Debugger + docs:** SDL en `ceres debug` (o documentar por qué no) y cerrar la
   documentación (07, 19, README, roadmap 28).

## 9. Comparativa rápida

| Criterio | A (backend tras interfaz) | B (SDL directo en el driver) |
| --- | --- | --- |
| Limpieza de capas | Alta (SDL fuera de core) | Media (driver crece) |
| Rapidez inicial | Media (interfaz que diseñar) | Alta |
| Build headless limpio | Sí (por construcción) | Sí (con opción + Pimpl) |
| Facilidad de testear | Alta | Media |
| Riesgo de rehacer abstracción | Bajo-Medio | Bajo |
| Fidelidad a "consola retro" | Se alcanza en Fase 2 | Desde el inicio |

**Recomendación:** empezar por la **Fase 0 (spike)**, y luego elegir **A** si se valora la separación
de capas y la testabilidad (consistente con el resto del proyecto), o **B** si se quiere llegar antes
a píxeles en pantalla.

> **Resultado:** elegida la **Propuesta A**, con códigos de tecla a 32 bits y framebuffer a píxeles
> desde el inicio (ver [Decisión](#decisión-consensuada)).

## Related pages

- [I/O devices and ports](07-IO-Devices-and-Ports.md) — los dispositivos (terminal, teclado, ratón, framebuffer, DMA).
- [Roadmap educativa y retro](28-Roadmap-Educativo-y-Retro.md) — Fase 3 "consola retro" y Fase 5 "entrada inmediata".
- [The debugger](22-Debugger.md) — el REPL que comparte stdin con la futura ventana.

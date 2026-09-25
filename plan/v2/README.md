# Máquina Ceres v2: plan de implementación para agentes

Este directorio es el plan ejecutable del refactor «Máquina Ceres v2». Está escrito para que un agente
(o una persona) lo siga **tarea a tarea**, sin tener que reconstruir el contexto de la conversación en la
que se diseñó. El diseño razonado, con comparativas y diagramas, está en el informe:
<https://claude.ai/artifact/3rdLFDYfCbjMtZfHr7tweN> (versión 4 o posterior). Si el informe y estos
archivos no coinciden, **manda este directorio**.

## Archivos

| Archivo | Para qué sirve | Cuándo leerlo |
| --- | --- | --- |
| [README.md](README.md) | Protocolo de trabajo, repos, comandos, reglas | Siempre, al empezar una sesión |
| [SPEC.md](SPEC.md) | La especificación normativa: mapa de memoria, relojes, perfiles, MMIO, IRQ, registros, opcodes, niveles | La sección que toque la tarea |
| [DECISIONS.md](DECISIONS.md) | Decisiones cerradas y pendientes (con la opción por defecto) | Antes de una tarea marcada `HUMANO` o que dependa de una decisión |
| [STATUS.md](STATUS.md) | Estado de cada tarea y el commit que la cerró | Al elegir la siguiente tarea y al terminarla |
| [phases/](phases/) | Un archivo por fase (`F00` … `F16`) con sus tareas | La fase de la tarea elegida |
| [tools/gen_status.js](tools/gen_status.js) | Regenera STATUS.md desde las fases, conservando estados y commits | Tras añadir, quitar o renombrar tareas |

Cada tarea de fase sigue la misma plantilla: repos, dependencias, decisiones, secciones de SPEC, archivos,
pasos, criterios de aceptación (casillas), verificación y mensaje de commit. Una fase empieza con su
«Punto de partida» (lo medido en el código) y termina con «Cierre de la fase» y «Notas».

## Qué se construye (resumen de una pantalla)

- **Memoria**: RAM `0x00000000–0x7FFFFFFF` (hasta 2 GiB), VRAM `0xA0000000–0xDFFFFFFF` (hasta 1 GiB), MMIO
  `0xFF000000–0xFFFFFFFF`. El resto, vacío (fallo). Memoria del host reservada de forma perezosa.
- **Relojes**: CPU y GPU con frecuencia fija y configurable; tiempo virtual; ciclos por instrucción;
  planificador de eventos; ocho perfiles de máquina (`micro` … `custom`).
- **ISA**: sigue siendo de 32 bits; 44 opcodes nuevos de 64 bits sobre **pares de registros**
  (`x0–x6`, `d0–d7`). Campos de instrucción con desplazamientos y máscaras (no uniones).
- **Dispositivos**: registros MMIO **sólo de 32 bits**; `IODevice` = `read(u32)`/`write(u32)`; tabla de
  registros declarativa; **un dispositivo por archivo** con `.h` y `.cpp`; un test por dispositivo.
- **E/S**: el programa **nunca** usa el terminal del host. `stdout`/`stdin` van a un **terminal virtual**
  dibujado en la ventana SDL. El host sólo recibe logs (registro de depuración) y diagnósticos.
- **Vídeo**: GPU con siete niveles: V0 Terminal, V1 Framebuffer, V2 Retro 2D, V3 Arcade 2D, V4 Vectorial 2D,
  V5 3D, V6 3D programable (opcional). Ejecutor software (referencia) y hardware (`SDL_GPU`).
- **Audio**: cinco niveles: A0 Tono MIDI, A1 PSG, A2 FM + secuenciador MIDI, A3 Sampler, A4 Audio digital.
  Síntesis siempre software y determinista.
- **Sin retrocompatibilidad**: se migran todos los tests, ejemplos y librerías; no hay capas de compatibilidad.

## Repositorios

| Repo | Ruta | Rama | Nombre en GitNexus |
| --- | --- | --- | --- |
| CeresASM (VM, ensamblador, enlazador, debugger, dispositivos) | `D:\Projects\CeresASM` | `main` | `Ceres-VM` |
| Ceres-C (compilador) | `D:\Projects\Ceres-C` | `main` | `Ceres-C` |
| Ceres STDLIB (biblioteca de C) | `D:\Projects\Ceres Projects\Ceres STDLIB` | `main` | — |

La STDLIB encuentra a los otros dos como hermanos (`../../Ceres-C`, `../../CeresASM`).

## Comandos

```bash
# CeresASM: build y tests (unitarios + e2e)
cmake --build "D:/Projects/CeresASM/Ceres/build/gcc" --config Release
ctest -C Release --test-dir "D:/Projects/CeresASM/Ceres/build/gcc" --output-on-failure

# CeresASM: binario con ventana (SDL) y copia a la raíz, que es el que usan los tests de los otros repos
cmake --build "D:/Projects/CeresASM/Ceres/build/gcc-sdl-ipo" --target ceres --config Release
cp "D:/Projects/CeresASM/Ceres/build/gcc-sdl-ipo/bin/Release/ceres.exe" "D:/Projects/CeresASM/ceres.exe"

# CeresASM: benchmark del intérprete (puerta de rendimiento)
cmake --build "D:/Projects/CeresASM/Ceres/build/gcc-ipo" --target ceres_vm_benchmarks --config Release   # binario en build/gcc-ipo/bin/Release

# Ceres-C
cmake --build "D:/Projects/Ceres-C/build/gcc" --config Release
ctest -C Release --test-dir "D:/Projects/Ceres-C/build/gcc" --output-on-failure

# STDLIB (unos 3 minutos; -Test <nombre> para uno, -Levels 0 para un nivel, -Update reescribe .expected)
powershell -File "D:/Projects/Ceres Projects/Ceres STDLIB/tools/runtests.ps1"
```

Si un comando no existe tal cual (nombre de target, ruta de salida), búscalo en `CMakePresets.json` o
`CMakeLists.txt` del repo y **corrige este README en el mismo commit**.

## Protocolo de trabajo

### 1. Elegir la tarea

1. Abre [STATUS.md](STATUS.md). La siguiente tarea es la **primera `TODO` cuyas dependencias estén todas
   en `DONE`**, respetando el orden de las fases. Las vías paralelas (64 bits, audio) se pueden intercalar
   cuando sus dependencias lo permiten.
2. Si la tarea está marcada `HUMANO`, o depende de una decisión `PENDIENTE` en [DECISIONS.md](DECISIONS.md),
   **para y pregunta al usuario**, proponiendo la opción por defecto. No decidas por tu cuenta.
3. Marca la tarea `DOING` en STATUS.md (no hace falta commit sólo para eso).

### 2. Antes de tocar código

1. Lee la tarea completa en su archivo de fase y las secciones de [SPEC.md](SPEC.md) que cita.
2. **Refinamiento**: las tareas de las fases tardías son más gruesas a propósito. Si la tarea no nombra
   archivos concretos, o el código ya no es como describe, **actualiza primero el archivo de fase** con los
   archivos y pasos reales (en el mismo commit que la tarea o en uno previo `plan: refine Fx.y`).
3. En CeresASM y Ceres-C, ejecuta `impact` de GitNexus sobre cada símbolo que vayas a modificar
   (`repo: "Ceres-VM"` o `"Ceres-C"`). Si el riesgo es HIGH o CRITICAL, **díselo al usuario** antes de editar.
   Nota: `generateInstr`, `slotAddress` y `Expr` salen CRITICAL por nombres ambiguos; ahí las suites son la
   comprobación real.

### 3. Hacer la tarea

- Una tarea es **un commit por repo tocado**, y cada commit se sube en cuanto se hace (`git push origin main`).
- Sólo se añaden al commit los archivos de la tarea. **Nunca** `AGENTS.md` ni `CLAUDE.md`: GitNexus los
  modifica solo y no forman parte de ningún cambio.
- Si la tarea toca varios repos y rompe la interfaz entre ellos (mapa MMIO, E/S, ABI), haz los commits de
  los tres **seguidos**, sin dejar `main` roto entre repos más tiempo del necesario.
- Orden entre repos dentro de una tarea: **CeresASM → (copiar `ceres.exe`) → Ceres-C → STDLIB**.

### 4. Verificar

1. Ejecuta la verificación de la tarea (bloque «Verificación») y **todas** las suites de los repos tocados.
2. Antes de cada commit en CeresASM o Ceres-C: `detect_changes` de GitNexus; comprueba que sólo cambian los
   símbolos y flujos esperados.
3. Si una puerta de calidad de la fase aplica (rendimiento, determinismo, memoria del host), mídela y apunta
   el número en la sección «Notas» del archivo de fase.

### 5. Cerrar

1. Commit con mensaje en inglés, en el estilo de los repos, terminado con el identificador de la tarea:
   `Split the timer into its own device file (F1.3)`. Añade la línea de atribución que pida tu entorno.
2. Actualiza STATUS.md: estado `DONE` y hash(es) del commit. Si la tarea tocó CeresASM, incluye STATUS.md en
   ese mismo commit; si no, haz un commit pequeño en CeresASM `plan: mark Fx.y done`.
3. Cada 3–4 commits (y al final de cada fase): revisión con `ocr` (el CLI de open-code-review, modo normal,
   **no** el delegado) sobre los commits desde la última revisión. Corrige los hallazgos reales en un commit
   `Fix the OCR findings on <tema>` y di cuáles descartaste y por qué.

### Cuándo parar y preguntar

- La tarea es `HUMANO` o depende de una decisión pendiente.
- GitNexus da HIGH o CRITICAL en algo que no es un falso positivo conocido.
- Una suite falla y tras dos intentos razonables no sabes por qué.
- La tarea obliga a cambiar SPEC.md de forma que afecta a otras fases (cambia un número, un registro, un
  opcode). Propón el cambio; no lo apliques sin confirmación.
- Algo del plan contradice lo que ves en el código de forma importante.

## Trampas conocidas del entorno

- Los repos usan CRLF en el árbol de trabajo (`core.autocrlf=true`, `* text=auto`). Escribe archivos con la
  herramienta de escritura de archivos, no con heredocs ni `node -e` desde Bash: el shell colapsa las
  barras invertidas y un `\n` dentro de código C acaba como salto de línea real.
- **Nunca** recompiles `ceresc.exe` ni sustituyas `ceres.exe` mientras corre `runtests.ps1`: rompe la ejecución.
- Un test que se cuelga deja `ceres.exe` vivo: `taskkill /IM ceres.exe /F`.
- Hasta la fase F5, cualquier ejecución que compare fotogramas de texto necesita `CERES_HEADLESS=1` o
  `--terminal` (el `ceres.exe` con SDL abre ventana por defecto). Desde F5 se usa `--headless`.
- `CHECK_EQ(a, b)` evalúa dos veces sus argumentos al fallar: no pongas llamadas con efectos dentro.
- La STDLIB compila tests y ejemplos con `-Werror`, y Ceres-C comprueba los formatos de `printf`.
- Una `extern` de C sobre un nombre definido por el enlazador rompe `ceres asm` de todas las unidades de la
  librería: esos nombres se alcanzan desde asm.
- Los manejadores de fallo corren en la pila del sistema (4 KiB): nada de `printf` en ellos.

## Definición de terminado (global)

Una fase está terminada cuando:

- todas sus tareas están `DONE` en STATUS.md;
- las tres suites (`ctest` de CeresASM, `ctest` de Ceres-C y `runtests.ps1` de la STDLIB) pasan;
- las puertas de calidad de la fase se cumplen y sus números están en «Notas»;
- la documentación que la fase nombra está actualizada;
- se ha hecho la revisión `ocr` de cierre de fase.

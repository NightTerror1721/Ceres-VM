# Migración de `Ceres-ASM-old/` a `Ceres/`

El árbol nuevo está montado y construye desde el primer día, con los directorios todavía
vacíos. La idea es mover el código a trozos, en commits que compilen, sin dejar la rama rota
entre uno y otro.

## Construir

```sh
cmake --preset msvc && cmake --build --preset msvc-debug
cmake --preset gcc  && cmake --build --preset gcc-debug && ctest --preset gcc-debug
```

El preset `msvc` no fija la versión del generador a propósito: en esta máquina hay Visual
Studio 18 y en el CI hay 17, y CMake ya elige el más nuevo que encuentra. Los generadores de
VS 17 en adelante compilan x64 por defecto, que es lo único que este proyecto ha soportado de
verdad — sólo las configuraciones x64 tenían `stdcpp23` y el directorio de includes.

Al configurar, CMake dice qué falta:

```
-- Ceres 0.1.0 - C++23, GNU 15.2.0
--   por migrar  : core, vm, devices, asm, debug
--   ejecutable  : sin apps/cli/src/main.cpp todavía
```

## Las reglas del árbol nuevo

**`include/` es la API, `src/` no lo es.** Lo que esté bajo `libs/<x>/include/ceres/<x>/` lo
puede incluir cualquiera; lo que esté en `libs/<x>/src/` no lo ve nadie de fuera, y el
compilador lo hace cumplir. Al mover cada cabecera hay que decidir de qué lado cae. En la duda,
`src/`: subirla después es fácil, bajarla cuando ya hay quien la incluye no lo es.

**Los includes públicos van con ángulos y ruta completa.** `#include "vm/opcodes.h"` pasa a ser
`#include <ceres/core/isa/opcodes.h>`. Entre cabeceras privadas de la misma librería vale
`#include "lexer.h"` como hasta ahora.

**Nadie añade una dependencia sin escribirla.** Si `ceres-asm` necesita algo de `ceres-vm`, no
compila hasta que alguien ponga `ceres::vm` en el `DEPENDS` de `libs/asm/CMakeLists.txt`. Ése
es el punto de toda la separación: que sea una decisión y no un descuido.

**Los ficheros se descubren solos, de momento.** Mover un `.cpp` no exige tocar ningún
CMakeLists. Cuando la migración termine hay que cambiar los globs de `cmake/CeresLibrary.cmake`
por listas explícitas — un glob no sabe avisar de que alguien se dejó un fichero fuera.

---

## Mapa de destinos

### `libs/core` — el contrato compartido

| De `Ceres-ASM-old/src/` | A |
| --- | --- |
| `common/types.h` `assert.h` `config.h` `logs.h` `int24.h` `fixed_vector.h` `memory.h` `string_utils.h` | `libs/core/include/ceres/core/base/` |
| `vm/opcodes.h` `instructions.h` `registers.h` `fregisters.h` `address.h` `interrupts.h` `disassembler.h` | `libs/core/include/ceres/core/isa/` |
| `vm/program.h` | `libs/core/include/ceres/core/format/program.h` |
| `vm/program.cpp` | `libs/core/src/format/program.cpp` |
| `debug/debug_info.h` | `libs/core/include/ceres/core/format/debug_info.h` |
| `debug/debug_info.cpp` | `libs/core/src/format/debug_info.cpp` |
| *(nuevo)* constantes del mapa de memoria | `libs/core/include/ceres/core/format/memory_map.h` |

`common/config.h` define `forceinline` y lee `CERES_DEBUG`. La macro la sigue poniendo la
construcción, ahora desde `cmake/CeresSettings.cmake` y en el target público, para que llegue
también a quien incluya la cabecera.

### `libs/vm` — la máquina

| De | A |
| --- | --- |
| `vm/memory.h` *(sin las constantes del mapa)* | `libs/vm/include/ceres/vm/memory.h` |
| `vm/ceresvm.h` `interrupt_controller.h` `io_ports.h` `bios.h` | `libs/vm/include/ceres/vm/` |
| `vm/execution_engine.h` | `libs/vm/include/ceres/vm/` *(ver nota)* |
| `vm/ceresvm.cpp` `execution_engine.cpp` | `libs/vm/src/` |

`execution_engine.h` son 1 647 líneas de cabecera. Es el sitio donde preguntarse si la API de
`ceres-vm` es todo eso o sólo `CeresVM`; si es lo segundo, la cabecera baja a `src/` y en
`include/` queda lo que de verdad se usa desde fuera.

### `libs/devices` — los periféricos

| De | A |
| --- | --- |
| `vm/devices.h` | `libs/devices/include/ceres/devices/devices.h` |
| `vm/storage_devices.h` | `libs/devices/include/ceres/devices/storage_devices.h` |

### `libs/asm` — ensamblador y enlazador

Las 45 entradas de `assembler/` se van enteras. Reparto sugerido:

| | |
| --- | --- |
| **`include/ceres/asm/`** | `assembler.h` `object_linker.h` `linker.h` `errors.h` |
| **`src/`** | todo lo demás: `lexer` `parser` `token` `statement` `operand` `mnemonic` `instruction_info` `macro_table` `symbol_table` `scope` `const_expr*` `data_type*` `literal_*` `strings_pool` `binary_emitter` `object_file` `relocation*` `assembly_state` `identifier` `size` `common_defs` |

### `libs/debug` — el depurador

| De `debug/` | A |
| --- | --- |
| `debug_session.h` `debug_cli.h` `debug_server.h` | `libs/debug/include/ceres/debug/` |
| `json.*` `expression.*` `history.*` | `libs/debug/src/` |
| `debug_session.cpp` `debug_cli.cpp` `debug_server.cpp` | `libs/debug/src/` |
| `debug_info.*` | **no**: se va a `libs/core/src/format/` |

`json.h` dice de sí mismo que es «lo justo de JSON para hablar el protocolo». Mientras sólo lo
use el depurador se queda privado aquí; si algún día lo necesita otro, sube a `core/base/`.

### `apps/cli`

| De | A |
| --- | --- |
| `src/main.cpp` | `apps/cli/src/main.cpp` |

### Tests

| De `tests/` | A | Por qué |
| --- | --- | --- |
| `framework.h` `main.cpp` | `Ceres/tests/framework/` | El andamio, sin dependencias |
| `assemble_helper.h` | `Ceres/tests/e2e/` | Arrastra el ensamblador entero |
| `test_encoding` `test_language` `test_macros` `test_modules` `test_pipeline` `test_robustness` `test_devices` `test_debug_info` `test_program_file` `test_objects` | `Ceres/tests/e2e/` | Ensamblan fuente y la ejecutan |
| `test_vm` | `libs/vm/tests/` | Sólo necesita la máquina |
| `test_debugger` `test_expression` `test_history` `test_json` | `libs/debug/tests/` | `ceres-debug` ya depende de `asm` y `vm` |

Nueve de los quince ficheros pasan por `assemble_helper.h`, así que **partir las librerías no
parte los tests**. No hay que reescribirlos: son buenos. Sólo hay que llamarlos por lo que son.

`test_program_file.cpp` es el mejor candidato a convertirse en un test unitario de `core`: sólo
usa `assemble_helper` para fabricarse un `.cres`, y eso se puede construir a mano.

### Lo demás

| De | A |
| --- | --- |
| `Ceres-ASM-old/examples/` | `Ceres/examples/` |
| `Ceres-ASM-old/lib/call.casm` | `Ceres/stdlib/call.casm` |

`lib/` pasa a `stdlib/` para que no se confunda con `libs/`, que es otra cosa. Los dos son
candidatos a salir a su propio repositorio (`ceres-lang`) en la fase 04.

---

## Los dos cortes, con sus sitios exactos

Hay que aplicarlos **al mover**, no después, o el árbol nuevo hereda las mismas aristas.

**1. El ensamblador incluye el depurador.** Tres cabeceras de `assembler/` incluyen
`debug/debug_info.h`:

```
assembler/assembler.h
assembler/binary_emitter.h
assembler/object_linker.h
```

Como `debug_info` se va a `core`, los tres pasan a `#include <ceres/core/format/debug_info.h>`
y la arista desaparece sola.

**2. El enlazador conoce la RAM.** Dos usos de una constante que vive dentro de `vm::Memory`:

```
assembler/linker.cpp:416        memoryMap.textStart = vm::Memory::UnrestrictedSegmentStart;
assembler/object_linker.cpp:161 const u32 textStart = vm::Memory::UnrestrictedSegmentStart.value();
```

Hay que sacar las constantes del mapa de memoria de `vm/memory.h` a
`core/format/memory_map.h`, e incluirla desde los dos sitios. `Memory` — el array de bytes de
una máquina en marcha — se queda en `ceres-vm`, y `libs/asm/CMakeLists.txt` no gana ningún
`DEPENDS`.

Para comprobar que el corte está hecho no hace falta leer nada: si `ceres_asm` compila, está
hecho, porque su include path no contiene ni `debug/` ni `vm/`.

---

## Fuera del alcance de esta fase

Lo siguiente sigue apuntando al árbol viejo y hay que rehacerlo cuando `Ceres/` tenga código:

- **`.github/workflows/ci.yml`** — dos rutas de `msbuild`, la ruta del ejecutable de tests y una
  línea de `g++` que enumera `vm/*.cpp assembler/*.cpp debug/*.cpp`. Se sustituye entera por
  `cmake --preset` + `cmake --build --preset` + `ctest --preset`.
- **`tests/build.sh`** — los mismos globs y los dieciséis ficheros de test a mano. Desaparece.
- **`Ceres-ASM-old/*.vcxproj`** — 87 entradas por duplicado. Las genera CMake.
- **`editors/vscode-ceresasm/client/src/compilerPath.ts`** y **`server/src/compiler.ts`** —
  buscan `Ceres-ASM/src/ceres.exe` subiendo hasta ocho directorios. Ahora el binario sale en
  `Ceres/build/<preset>/bin/`.
- **`README.md`** — las instrucciones de construcción son la línea de `g++` y la ruta del
  `.vcxproj`.
- **`.gitnexus/`** — el índice está construido sobre las rutas viejas: `node .gitnexus/run.cjs
  analyze` cuando el árbol nuevo tenga código.

Dos trampas del `.gitignore` ya están resueltas, y conviene saber que existían:

- La regla `[Dd]ebug/` de Visual Studio se traga `Ceres/libs/debug/`, igual que se tragaba
  `Ceres-ASM/src/debug/`. Hay negaciones explícitas para ambas.
- La regla `/ceres` del binario compilado también capturaba el directorio `Ceres/`, porque en
  Windows git compara los patrones sin distinguir mayúsculas — el árbol entero era invisible
  para `git status`. Lo arregla un `!/Ceres/` con barra final, que sólo aplica a directorios.

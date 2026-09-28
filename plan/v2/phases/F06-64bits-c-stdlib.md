# F6 · 64 bits en Ceres-C y en la STDLIB

- **Tamaño**: L · **Depende de**: F3, F5, decisiones P10, P11 · **Vía**: 64 bits · **Repos**: Ceres-C, STDLIB, CeresASM (doc 24)
- **Objetivo**: `long long` compilado con las instrucciones nativas y `double` como binary64 real. **Hito 2.**
- **SPEC**: §6.4, §6.5, §6.7.

## Punto de partida

- Ceres-C (`docs/06-Known-Limitations.md`): `long long` se baja como par de palabras en memoria; `*` con `mul` y
  `mulh`; `/` y `%` llaman a `__cc_div64` (bucle emitido al final de `@text`); `E5002` para `switch` de 64 bits y
  builtins de 64 bits; `double` limitado a `float` con aviso, o binary64 por software con `-fsoft-double`
  (STDLIB `lib/soft-double/`, `include/ceres/f64.h`, `src/fconv64.c`).
- Ceres-C pasa un valor de 64 bits en dos registros **consecutivos** (no alineados).
- STDLIB: `Makefile` `SOFT_DOUBLE=1`, `CMakeLists.txt` `CERES_SOFT_DOUBLE`, presets `O2-sd`; `ns64.h` deprecado.

## Tareas

### F6.1 · ABI de pares

- **Repos**: CeresASM (doc 24), Ceres-C · **Depende de**: — · **Decisiones**: P11
- **Pasos**: actualiza `docs/24-Calling-Convention.md` con SPEC §6.7; en Ceres-C, asignación de argumentos y
  retorno de 64 bits a pares alineados (`x0`, `x1`; `d0`, `d1`), y pila de 8 bytes con alineación 4.
- **Aceptación**: [x] Tests de llamadas con mezclas `(int, long long)`, `(double, int, double)`, más de cuatro argumentos.
- **Commit** (uno por repo): `Pass 64-bit values in aligned register pairs (F6.1)`

### F6.2 · `long long` con instrucciones nativas

- **Repos**: Ceres-C · **Depende de**: F6.1
- **Pasos**:
  1. Codegen: cada operación de 64 bits se emite como `ldrd`, operación de SPEC §6.4 y `strd` sobre la
     representación en memoria actual (sin tocar aún el asignador de registros).
  2. Elimina `__cc_div64` y su emisión.
  3. Levanta `E5002` para `switch` de 64 bits (compara con `cmp64`) y añade builtins de 64 bits
     (`__builtin_clzll`, `ctzll`, `popcountll`) sobre `0xE7`.
- **Aceptación**: [x] Suite de Ceres-C en verde. [x] El ejemplo `35_int64.c` ocupa menos instrucciones (apúntalo en «Notas»).
- **Commit**: `Compile long long with the native 64-bit instructions (F6.2)`

### F6.3 · `double` binary64

- **Repos**: Ceres-C · **Depende de**: F6.1 · **Decisiones**: P10
- **Pasos**: tipo `double` (y `long double`) de 8 bytes; literales sin sufijo son `double`; promociones usuales y
  de argumentos variádicos (`float` → `double`); conversiones con `fcvt`; constantes con `.double` y
  `fldr.d [pc + …]`; `-fshort-double` hace `double` = `float`; elimina `-fsoft-double` y el aviso de «double es
  float»; formato de `printf` (W3005) con `%f` = `double`.
- **Aceptación**: [ ] Tests de aritmética, conversiones y variádicos. [ ] Docs 02 y 06 de Ceres-C actualizados.
- **Commit**: `Make double a real binary64 (F6.3)`

### F6.4 · STDLIB en doble

- **Repos**: STDLIB · **Depende de**: F6.2, F6.3
- **Pasos**: `printf`/`scanf`/`strtod` con `double`; `math.h` con funciones `double` nativas y las `float`
  (`sinf`…) como rápidas; `time_t` y relojes con instrucciones de 64 bits; elimina `f64.h`, `ns64.h`,
  `src/fconv64.c` si queda sin uso, `SOFT_DOUBLE`, `CERES_SOFT_DOUBLE`, presets `-sd` y `lib/soft-double`;
  `tests/f64_vectors.inc` pasa a probar `double` nativo.
- **Aceptación**: [ ] `runtests.ps1` en verde; `.expected` revisados a mano.
- **Commit**: `Use native double and 64-bit integers in the library (F6.4)`

### F6.5 · Pares en el asignador de registros

- **Repos**: Ceres-C · **Depende de**: F6.4
- **Pasos**: los valores de 64 bits viven en pares de registros (restricción de registro par) en lugar de en
  memoria; permitir inlining y llamadas de cola con valores de 64 bits.
- **Aceptación**: [ ] Suites en verde. [ ] Menos instrucciones en los ejemplos de 64 bits (apúntalo).
- **Commit**: `Allocate 64-bit values to register pairs (F6.5)`

### F6.6 · Documentación

- **Repos**: Ceres-C, STDLIB · **Depende de**: F6.5
- **Commit**: `Document native 64-bit types (F6.6)`

## Cierre de la fase

- [ ] Suites en verde. [ ] Revisión `ocr` en Ceres-C y STDLIB. [ ] Hito 2 anunciado.

## Notas

- **F6.1**: `assignArgSlots` alinea el par al registro par de su banco y no rellena el impar que salta; si no
  queda par, el valor va a dos palabras de pila y el banco se da por gastado (una palabra posterior también va a
  la pila), como AAPCS. `(double, int, double)` se prueba sobre el reparto (`d0`, `r0`, `d1`); con el `double`
  nativo lo prueba también F6.3 de extremo a extremo. Arreglo de paso (viene de F5): los tests de Ceres-C que
  enlazan con `ceresc --run` (el grupo `prebuilt` y uno de `e2e`) no pasaban `--headless`, y un `ceres run` con
  ventana espera una tecla al acabar; el primero se colgaba. Ahora pasan `--headless --speed max`.
- **F6.2**: en vez de emitir `ldrd`/op/`strd` sobre el par direccionado de antes, el valor de 64 bits pasa a ser
  un temporal de par del IR (opcode nuevo `Wide`, `ir_instr.h`), que por ahora vive siempre en un campo de 8
  bytes del marco: la forma que pide el paso 1 («sobre la representación en memoria») y la que F6.5 sólo tiene
  que llevar a registros. Los pares de trabajo son `x2` (`r4:r5`) y `x3` (`r6:r7`, que sale de la reserva en una
  función con operaciones de 64 bits). `and`/`or`/`xor`/`not` de 64 bits no tienen instrucción y son dos de 32.
  `35_int64.c`: 2283 instrucciones a -O1 antes, 1142 ahora (-O0: 7333 → 1903; -O2: 2295 → 1136). El inliner
  sigue rechazando funciones con valores de 64 bits (F6.5). En la STDLIB, `timerdemo` imprimía un `tick 6` de
  más: un `printf` cuesta ~2300 ciclos frente a un periodo de 3000 y la fase de los sondeos cambió; sus plazos
  se multiplicaron por 10 (lib@3319b90).
- **F6.3**: `double` y `long double` son `Type::Double` de 8 bytes; un literal sin sufijo es `double`, y un
  `float` sube a `double` en `...` (`va_arg(ap, float)` se rechaza, como en C). Las conversiones son `fcvt`; las
  constantes, `li64` con los bits. `>`/`>=` de dobles se preguntan como `<`/`<=` con los operandos cambiados:
  un NaN deja Carry y Zero a 0, y `jab`/`jae` lo leerían como cierto. Un parámetro `double` ocupa un campo del
  banco de coma flotante. Las funciones matemáticas integradas (`__builtin_sqrt`…) son polimórficas: la
  instrucción `.d` para un `double`. `-fshort-double` (define `__CERES_SHORT_DOUBLE__`) sustituye a
  `-fsoft-double`, y W2001 se retira. Docs 02, 05, 06, 09 y 11 de Ceres-C al día.
- **F6.4**: `<math.h>` tiene las dos familias: los nombres estándar en `double` (`src/math64*.c`, nuevos, y las
  de una instrucción en `asm/math_ops.casm`) y la familia `float` con sufijo `f` (el código de antes,
  renombrado). Con `-fshort-double` los nombres estándar son macros de los `f`: una biblioteca sola sirve a los
  dos modos. `printf`/`scanf`/`strtod`/`difftime` en `double`; fuera `f64.h`, `ns64.h`, `SOFT_DOUBLE`,
  `CERES_SOFT_DOUBLE`, los presets `-sd` y `lib/soft-double` (`fconv64.c` se queda: es el `printf` de `double`).
  `test_f64` pasa los 4000 vectores por el `double` nativo. Precisión medida en el host contra referencias de
  80 bits y de 400 dígitos: `exp`, `exp2`, `log*`, `pow`, `cbrt`, `hypot`, `sin`/`cos` (reducción exacta para
  todo `double`), `atan`, `asin`, `acos`, `expm1`, `log1p` y `erf` dentro de ~1 ulp (`exp`/`pow` acaban en
  doble-doble: el más cercano salvo casi-empates); `tan`, `atan2`, hiperbólicas y `erfc` 2-4; `tgamma` ≤ 5;
  `lgamma` con x < 0 pierde precisión relativa junto a sus ceros (el error absoluto sigue en ~1e-16).
  `test_math64` lo comprueba contra `Math.*` de node y contra referencias de 100 dígitos. Arreglo de paso en
  Ceres-C (cc@992e20f): el nombre C `AT` salía como el registro `at`; los alias de registro se comparan ahora
  sin distinguir mayúsculas. `timer_wait_until_ns64` conserva su nombre.

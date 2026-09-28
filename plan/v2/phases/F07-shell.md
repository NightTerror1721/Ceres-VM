# F7 · Shell de Ceres

- **Tamaño**: M · **Depende de**: F5 · **Repos**: CeresASM, STDLIB
- **Objetivo**: arrancar la máquina y tener un prompt desde el que moverse por el disco y ejecutar programas. **Hito 3.**
- **SPEC**: §5.7 (SystemControl `Command` 3, `LoadPath`, `LoadArgs`), §10 (`--shell`).

## Tareas

### F7.1 · Cargar y ejecutar desde el programa

- **Repos**: CeresASM · **Depende de**: —
- **Archivos**: `devices/system/system_control.{h,cpp}` (registros y `LoadRequest`), `vm/ceresvm.{h,cpp}`
  (`requestLoad`), `devices/terminal/terminal.{h,cpp}` y `line_discipline.{h,cpp}` (la sesión que pasa de un
  programa a otro), `driver/src/machine_runner.{h,cpp}`, `driver/src/driver.cpp`, `driver/src/command.cpp`;
  tests `driver/tests/test_load.cpp`, `test_system_control.cpp`, `test_terminal.cpp`, `test_vm.cpp`.
- **Pasos**:
  1. SystemControl: `LoadPath` y `LoadArgs`; el comando 3 carga el `.cres` de la ruta HostFs, reinicia la
     máquina con esos argumentos y lo ejecuta.
  2. `--shell`: cuando el programa termina, el runner vuelve a cargar el shell y le pasa el código de salida.
  3. `ceres run` sin programa carga `<sysroot>/bin/shell.cres` (ruta configurable; error claro si no existe).
- **Aceptación**: [x] Test e2e: un programa lanza otro y se vuelve al primero con el código de salida.
- **Commit**: `Load and run a program from inside the machine (F7.1)`

### F7.2 · El shell

- **Repos**: STDLIB (y CeresASM: el aviso de una carga fallida nombra el programa, no la ruta del host) · **Depende de**: F7.1
- **Archivos**: `bin/shell/shell.c`; `src/ceres/run.c` e `include/ceres/sys.h` (`sys_run`, el comando 3);
  `CMakeLists.txt` (paso 5: `<build>/bin/shell.cres`, instalado en `<prefix>/bin`), `tools/install.ps1`,
  `tools/common.ps1` (`Build-Program`), `tools/runtests.js` y `tools/runtests.ps1` (paso del shell);
  `tests/shell/` (sesiones `.type`/`.expected`/`.stderr`/`.status`, `files/` y `greet.c`); `tests/test_sys.c`.
- **Pasos**: prompt `ceres:<dir>>`; comandos `help`, `ls`, `cd`, `cat`, `run`, `clear`, `mem`, `time`, `info`,
  `reset`, `exit` (`play` se añade en F9 y F11); historial por el terminal virtual; errores claros.
- **Aceptación**: [x] Tests con `--type` que recorren cada comando y comparan el transcript.
- **Commit**: `Add the Ceres shell (F7.2)`

### F7.3 · Documentación

- **Repos**: CeresASM, STDLIB
- **Commit**: `Document the shell (F7.3)`

## Notas

- **F7.1**: el comando 3 lee de la RAM, en el mismo `str`, la ruta (`LoadPath`) y el bloque de `LoadArgs`, que
  lleva una tercera palabra, `envp` (0: el entorno del programa actual; SPEC 5.7 actualizado), y lo pasa al host
  (`SystemControlDevice::LoadCallback`). El runner resuelve la ruta con HostFs, lee y comprueba el `.cres`
  (`CeresVM::requestLoad`: que quepa con sus argumentos) y la máquina arranca con él como en un reset; si algo
  falla, el programa sigue tras su `str` (semántica de `execve`) y el log del host dice por qué. Al cambiar de
  programa, el terminal conserva la pantalla (celdas, scrollback y cursor, si la geometría no cambió) y el
  historial de la disciplina de línea (`TerminalDevice::Session`); el resto de dispositivos se reinicia. Con
  `--shell`, o con `ceres run` sin programa, al acabar un programa que no es el shell éste vuelve con el entorno
  de aquél más `CERES_STATUS=<código>`: así el shell puede guardar su estado (directorio actual) en el entorno
  que da al hijo. El shell es `<sysroot>/bin/shell.cres`, con `--sysroot` o `CERES_SYSROOT`; en modo shell el
  directorio del host es el actual si no hay `--host-dir`. Un fallo no atendido en el programa se informa (log y
  pantalla) antes de volver al shell.
- **F7.2**: el shell es un programa normal (`bin/shell/shell.c`, enlazado con el archivo a -O2). Guarda su
  directorio en el `PWD` que da al hijo y lo recupera al volver; el historial y la pantalla los conserva el
  terminal. Un programa también se ejecuta por su nombre (`snake`, `snake.cres`). Saluda cuando la pantalla está
  limpia (arranque o `reset`) y, al volver de un programa que acabó con estado distinto de 0, dice
  `(exit status N)`. `reset` es el comando 2 (reinicio completo: pantalla e historial limpios); `exit [n]` apaga la
  máquina con ese estado; el fin de la entrada (Ctrl+D, o el final de `--type`) también sale. Ctrl+C en el prompt
  descarta la línea. `mem` redondea a KiB/MiB para que su salida no cambie con cada byte del programa. El
  historial no se puede probar con `--type` (en modo cocinado ESC es la tecla Esc; las flechas necesitan `--keys`):
  lo cubre el test del terminal en CeresASM. Los dos runners construyen el shell en `build/shell/bin`, copian
  `tests/shell/files` a `build/shell/host`, compilan ahí `tests/shell/*.c` (en `games/`) y teclean cada sesión con
  `--rtc` fijo; `.gitattributes` deja esos ficheros sin conversión de fin de línea, porque `ls` enseña tamaños.
  De paso: `tools/install.ps1 -Prefix` con una ruta absoluta fallaba (`Join-Path` de dos rutas absolutas).
- **F7.3**: CeresASM `docs/36-Shell-and-Program-Loading.md` (nueva: el comando 3, `--shell`, `--sysroot`, qué
  sobrevive al cambio de programa y los comandos del shell), `07` (registros `LoadPath`/`LoadArgs` y comando 3),
  `16` (`run` sin programa, `--shell`, `--sysroot`), el índice y el README raíz (que aún citaba `--memory`). STDLIB:
  README (sección «The shell», `make install` con `bin/shell.cres`, las sesiones de `tests/shell`) y la ayuda del
  Makefile.
- **Cierre de F7 (hito 3)**: `ctest` de CeresASM 11/11, Ceres-C 11/11 y la STDLIB 315 comprobaciones (104 tests ×
  3 niveles, cabeceras, ejemplos y las 3 sesiones del shell) con los dos runners. Revisión (delegada) de CeresASM
  e1f6f42..4294245 y la STDLIB 2daf707..197a0b9. Hallazgos reales: con el comando 3 es el programa quien elige
  el fichero, y el cargador dimensiona sus búferes por la cabecera, así que un `.cres` corrupto podía tumbar el
  host con `bad_alloc` desde el `str` del programa; ahora se rechaza antes de leerlo un fichero mayor que la RAM
  o más corto de lo que dice su cabecera, y cualquier excepción del cargador es una carga fallida
  (asm@171b63c, con test). En el shell, un `PWD` demasiado largo dejaba el directorio a medio escribir, y un
  Ctrl+C pulsado durante un comando daba un prompt doble (lib@2af4879; la sesión `errors` teclea uno).
  Descartados: que un programa lanzado desde el shell pueda leer lo tecleado por adelantado para el shell es lo
  que hace un terminal real; tras `reset` el shell conserva el entorno con que se cargó (y un `CERES_STATUS`
  viejo), pero con la pantalla limpia saluda en vez de repetir el estado.

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

- **Repos**: STDLIB · **Depende de**: F7.1
- **Archivos**: crear `tools/shell/shell.c` (o `bin/shell/`, según encaje con el build) y su regla de build e instalación en `sysroot/bin`.
- **Pasos**: prompt `ceres:<dir>>`; comandos `help`, `ls`, `cd`, `cat`, `run`, `clear`, `mem`, `time`, `info`,
  `reset`, `exit` (`play` se añade en F9 y F11); historial por el terminal virtual; errores claros.
- **Aceptación**: [ ] Tests con `--type` que recorren cada comando y comparan el transcript.
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

# F7 · Shell de Ceres

- **Tamaño**: M · **Depende de**: F5 · **Repos**: CeresASM, STDLIB
- **Objetivo**: arrancar la máquina y tener un prompt desde el que moverse por el disco y ejecutar programas. **Hito 3.**
- **SPEC**: §5.7 (SystemControl `Command` 3, `LoadPath`, `LoadArgs`), §10 (`--shell`).

## Tareas

### F7.1 · Cargar y ejecutar desde el programa

- **Repos**: CeresASM · **Depende de**: —
- **Pasos**:
  1. SystemControl: `LoadPath` y `LoadArgs`; el comando 3 carga el `.cres` de la ruta HostFs, reinicia la
     máquina con esos argumentos y lo ejecuta.
  2. `--shell`: cuando el programa termina, el runner vuelve a cargar el shell y le pasa el código de salida.
  3. `ceres run` sin programa carga `<sysroot>/bin/shell.cres` (ruta configurable; error claro si no existe).
- **Aceptación**: [ ] Test e2e: un programa lanza otro y se vuelve al primero con el código de salida.
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

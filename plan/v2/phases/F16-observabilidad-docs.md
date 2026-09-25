# F16 · Observabilidad y documentación (transversal)

- **Tamaño**: M · **Depende de**: cada tarea, de la fase que la habilita · **Repos**: CeresASM, STDLIB
- **Objetivo**: que cada pieza se entienda, se observe y se reproduzca. Estas tareas se hacen cuando su
  dependencia está lista; no bloquean a las demás fases.

## Tareas

### F16.1 · Captura y repetición de fotogramas · depende de F10
- **Pasos**: `.cgcap` con los paquetes de un fotograma y las páginas de VRAM que tocan; `ceres gpu-replay` lo
  reproduce en software y vuelca el resultado.
- **Commit**: `Capture and replay GPU frames (F16.1)`

### F16.2 · Visores · depende de F8 y F9
- **Pasos**: debugger `gpu dump-surface <n> <archivo.png>`, visor de tiles, OAM y paletas; osciloscopio por voz de audio (texto o PNG).
- **Commit**: `Add VRAM and audio viewers to the debugger (F16.2)`

### F16.3 · Estadísticas por fotograma · depende de F5
- **Pasos**: ciclos de CPU y GPU por fotograma, fotogramas perdidos, velocidad efectiva, en `ceres profile` y en la barra de estado.
- **Commit**: `Report per-frame statistics (F16.3)`

### F16.4 · Instantáneas por páginas · depende de F4
- **Pasos**: los snapshots del debugger y la ejecución reversible guardan sólo las páginas de RAM y VRAM tocadas,
  más el estado de reloj, planificador y dispositivos (`saveState`/`loadState`).
- **Commit**: `Snapshot only touched pages (F16.4)`

### F16.5 · Doc 07 generado · depende de F1.8
- **Pasos**: herramienta que genera las tablas de registros de `docs/07` desde las `RegisterMap`; test que falla si el doc está desactualizado.
- **Commit**: `Generate the device register tables from the code (F16.5)`

### F16.6 · Tutoriales y wiki · depende de F5 (y se repite al cerrar cada hito)
- **Pasos**: un tutorial por nivel de vídeo y de audio en CeresASM `docs/` y en la Ceres Wiki; `docs/28` actualizado.
- **Commit**: `Add the tutorial for <nivel> (F16.6)`

## Notas

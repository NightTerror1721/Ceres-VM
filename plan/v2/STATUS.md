# Estado de las tareas

Estados: `TODO`, `DOING`, `DONE`, `BLOCKED` (di por qué en «Notas» de la fase). Una tarea `HUMANO` necesita
al usuario. «Depende de» repite lo que dice el archivo de fase; si no coinciden, manda el archivo de fase.
Una tarea sin dependencias explícitas depende de la anterior de su fase; la primera, de lo que pide la fase.
Al cerrar una tarea, pon `DONE` y los commits como `repo@hash` (`asm@…` CeresASM, `cc@…` Ceres-C, `lib@…` STDLIB).
Tras añadir o renombrar tareas en una fase: `node plan/v2/tools/gen_status.js` (conserva estados y commits).

## F0 · Especificación y medición

Archivo: [phases/F00-especificacion.md](phases/F00-especificacion.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F0.1 | Medir el intérprete por clase de instrucción | — | DONE | asm@30d4e55 |
| F0.2 | Medir la presentación y el arranque con memoria grande | F0.1 | DONE | asm@b2441a1 |
| F0.3 | Prototipo de SDL_GPU | — | TODO |  |
| F0.4 | Prototipo del terminal ANSI y de un operador FM | — | TODO |  |
| F0.5 | Cerrar decisiones con el usuario · **HUMANO** | F0.1, F0.2 | TODO |  |
| F0.6 | Calibrar la tabla de ciclos · **HUMANO** | F0.1 | TODO |  |
| F0.7 | Enlazar el plan desde la documentación | — | DONE |  |

## F1 · Reorganización de dispositivos y del núcleo

Archivo: [phases/F01-dispositivos-y-nucleo.md](phases/F01-dispositivos-y-nucleo.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F1.1 | Separar `IODevice` y añadir `RegisterMap` | fase: F0 (F0.5 cerrada para P05 y P06) | TODO |  |
| F1.2 | `libs/devices` como biblioteca compilada y con carpetas por grupo | F1.1 | TODO |  |
| F1.3 | Partir `devices.h`: sistema y terminal | F1.2 | TODO |  |
| F1.4 | Partir `input_devices.h` | F1.2 | TODO |  |
| F1.5 | Partir `storage_devices.h` | F1.2 | TODO |  |
| F1.6 | Migrar a 32 bits todos los accesos a MMIO (tres repos) | F1.5 | TODO |  |
| F1.7 | Bus de 32 bits: `read`/`write` y `FaultReason` | F1.6 | TODO |  |
| F1.8 | Tabla de registros en cada dispositivo, `dev` y `--strict-mmio` | F1.7 | TODO |  |
| F1.9 | Un test por dispositivo | F1.8 | TODO |  |
| F1.10 | Campos de instrucción declarativos y serialización little-endian | fase: F0 (F0.5 cerrada para P05 y P06) | TODO |  |
| F1.11 | Comprobación automática de «un dispositivo por archivo» | F1.9 | TODO |  |
| F1.12 | Documentación de dispositivos y bus | F1.8 | TODO |  |

## F2 · Relojes y planificador de eventos

Archivo: [phases/F02-relojes.md](phases/F02-relojes.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F2.1 | Tabla de ciclos y contador | F1.10 | TODO |  |
| F2.2 | Planificador de eventos | F2.1 | TODO |  |
| F2.3 | Todos los dispositivos al planificador; halt por eventos | F2.2 | TODO |  |
| F2.4 | Timer v2 y `CpuClockHz` | F2.3 | TODO |  |
| F2.5 | Runner por tiempo virtual, `Pacer` y `--speed` | F2.4 | TODO |  |
| F2.6 | Entrada sellada, `--record` y `--replay` | F2.5 | TODO |  |
| F2.7 | Perfilador y debugger en ciclos | F2.3 | TODO |  |
| F2.8 | STDLIB en tiempo virtual | F2.4 | TODO |  |
| F2.9 | Test de determinismo y documentación | F2.6, F2.8 | TODO |  |

## F3 · ISA de 64 bits

Archivo: [phases/F03-isa-64.md](phases/F03-isa-64.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F3.1 | Opcodes, subcampos y fallos de decodificación | F1.10 | TODO |  |
| F3.2 | Banco float con bits en crudo | F3.1 | TODO |  |
| F3.3 | Ensamblador: pares, mnemónicos y datos de 64 bits | F3.1 | TODO |  |
| F3.4 | VM: enteros de 64 bits | F3.2, F3.3, F2.1 (ciclos) | TODO |  |
| F3.5 | VM: dobles, conversiones y memoria | F3.4 | TODO |  |
| F3.6 | Debugger y documentación | F3.5 | TODO |  |

## F4 · Memoria, mapa nuevo y perfiles

Archivo: [phases/F04-memoria-mapa-perfiles.md](phases/F04-memoria-mapa-perfiles.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F4.1 | Reserva perezosa de la RAM y 2 GiB | fase: F2 | TODO |  |
| F4.2 | VRAM y regiones del mapa físico | F4.1 | TODO |  |
| F4.3 | Mapa MMIO por grupos e IRQ nuevas (tres repos) | F4.2 | TODO |  |
| F4.4 | Registro de depuración y `HostLog` | F4.3 | TODO |  |
| F4.5 | Perfiles de máquina | F4.4 | TODO |  |
| F4.6 | Documentación | F4.5 | TODO |  |

## F5 · La ventana es el ordenador: GPU V0–V1 y terminal virtual

Archivo: [phases/F05-ventana-v0-v1-terminal.md](phases/F05-ventana-v0-v1-terminal.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F5.1 | Interfaces del host y división del backend SDL (sin cambiar comportamiento) | fase: F4, decisiones P03, P07, P08, P09 | TODO |  |
| F5.2 | Núcleo de la GPU, pantalla y VBlank | F5.1 | TODO |  |
| F5.3 | Plano de texto (V0) | F5.2 | TODO |  |
| F5.4 | Plano bitmap y motor de copia (V1) | F5.3 | TODO |  |
| F5.5 | Presentación por VBlank y ventana | F5.4 | TODO |  |
| F5.6 | Terminal virtual y pantalla de fallo | F5.3 | TODO |  |
| F5.7 | Salidas sin ventana y entrada guionizada | F5.6 | TODO |  |
| F5.8 | Retirar los dispositivos de texto y píxeles antiguos | F5.5, F5.7 | TODO |  |
| F5.9 | Ceres-C: tests y ejemplos sin stdout del host | F5.7 | TODO |  |
| F5.10 | STDLIB: cabeceras de vídeo y terminal | F5.8 | TODO |  |
| F5.11 | STDLIB: tests y ejemplos sin ventana | F5.10, F5.9 | TODO |  |
| F5.12 | Puerta «host limpio» y documentación | F5.11 | TODO |  |

## F6 · 64 bits en Ceres-C y en la STDLIB

Archivo: [phases/F06-64bits-c-stdlib.md](phases/F06-64bits-c-stdlib.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F6.1 | ABI de pares | fase: F3, F5, decisiones P10, P11 | TODO |  |
| F6.2 | `long long` con instrucciones nativas | F6.1 | TODO |  |
| F6.3 | `double` binary64 | F6.1 | TODO |  |
| F6.4 | STDLIB en doble | F6.2, F6.3 | TODO |  |
| F6.5 | Pares en el asignador de registros | F6.4 | TODO |  |
| F6.6 | Documentación | F6.5 | TODO |  |

## F7 · Shell de Ceres

Archivo: [phases/F07-shell.md](phases/F07-shell.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F7.1 | Cargar y ejecutar desde el programa | fase: F5 | TODO |  |
| F7.2 | El shell | F7.1 | TODO |  |
| F7.3 | Documentación | F7.2 | TODO |  |

## F8 · V2: Retro 2D

Archivo: [phases/F08-v2-retro.md](phases/F08-v2-retro.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F8.1 | Paletas y capas de tiles | F5 y el commit de «Antes de empezar» | TODO |  |
| F8.2 | Capa afín | F8.1 | TODO |  |
| F8.3 | Sprites | F8.2 | TODO |  |
| F8.4 | Tabla de líneas y efectos raster | F8.3 | TODO |  |
| F8.5 | STDLIB, herramienta y ejemplos | F8.4 | TODO |  |
| F8.6 | Documentación | F8.5 | TODO |  |

## F9 · Audio A0–A2: tono, PSG, FM y secuenciador MIDI

Archivo: [phases/F09-audio-a0-a2.md](phases/F09-audio-a0-a2.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F9.0 | Decidir la FM · **HUMANO** | fase: F5 (y F2 para el tiempo virtual) | TODO |  |
| F9.1 | Esqueleto del audio en tiempo virtual | F5, F2 | TODO |  |
| F9.2 | A0 Tono con cola de notas | F9.1 | TODO |  |
| F9.3 | A1 PSG | F9.2 | TODO |  |
| F9.4 | A2 FM | F9.3, F9.0 | TODO |  |
| F9.5 | Secuenciador MIDI y banco FM | F9.4 | TODO |  |
| F9.6 | STDLIB y herramienta | F9.5 | TODO |  |
| F9.7 | Documentación | F9.6 | TODO |  |

## F10 · V3: Arcade 2D

Archivo: [phases/F10-v3-arcade.md](phases/F10-v3-arcade.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F10.1 | Procesador de comandos | fase: F8 | TODO |  |
| F10.2 | Motor 2D | F10.1 | TODO |  |
| F10.3 | Capas y sprites de V3 | F10.2 | TODO |  |
| F10.4 | Retirar el blitter y STDLIB | F10.3 | TODO |  |
| F10.5 | Ejemplos y documentación | F10.4 | TODO |  |

## F11 · Audio A3–A4: sampler y audio digital

Archivo: [phases/F11-audio-a3-a4.md](phases/F11-audio-a3-a4.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F11.0 | Códecs y banco de muestras · **HUMANO** | fase: F9 | TODO |  |
| F11.1 | Voces del sampler | F9 | TODO |  |
| F11.2 | Efectos del sampler | F11.1 | TODO |  |
| F11.3 | Bancos de instrumentos | F11.1, F11.0 (P14) | TODO |  |
| F11.4 | Flujos PCM | F11.1, F11.0 (P13) | TODO |  |
| F11.5 | STDLIB | F11.3, F11.4 | TODO |  |
| F11.6 | Documentación | F11.5 | TODO |  |

## F12 · Modo hardware para V0–V3

Archivo: [phases/F12-modo-hardware.md](phases/F12-modo-hardware.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F12.0 | Alcance y orden · **HUMANO** | fase: F10, decisión P15, spike F0.3 | TODO |  |
| F12.1 | `SdlGpuExecutor` y selección de ejecutor | F12.0 | TODO |  |
| F12.2 | Coherencia de VRAM y caché de recursos | F12.1 | TODO |  |
| F12.3 | Shaders precompilados y compositor | F12.2 | TODO |  |
| F12.4 | Motor 2D en la GPU | F12.3 | TODO |  |
| F12.5 | Conformidad y métricas | F12.4 | TODO |  |
| F12.6 | Documentación | F12.5 | TODO |  |

## F13 · V4: Vectorial 2D

Archivo: [phases/F13-v4-vectorial.md](phases/F13-v4-vectorial.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F13.1 | Trazados y comandos | fase: F12 | TODO |  |
| F13.2 | Rasterizador de cobertura en software | F13.1 | TODO |  |
| F13.3 | Pintura, transformaciones, recortes y grupos | F13.2 | TODO |  |
| F13.4 | Texto por contornos | F13.3 | TODO |  |
| F13.5 | V4 en hardware | F13.4 | TODO |  |
| F13.6 | STDLIB y documentación | F13.5 | TODO |  |

## F14 · V5: 3D de función fija

Archivo: [phases/F14-v5-3d.md](phases/F14-v5-3d.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F14.1 | Búferes, estado y matrices | fase: F12, F6 | TODO |  |
| F14.2 | Rasterizador 3D en software | F14.1 | TODO |  |
| F14.3 | Texturas y sombreado | F14.2 | TODO |  |
| F14.4 | V5 en hardware | F14.3 | TODO |  |
| F14.5 | STDLIB, ejemplos y documentación | F14.4 | TODO |  |

## F15 · V6: 3D programable (opcional)

Archivo: [phases/F15-v6-shaders.md](phases/F15-v6-shaders.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F15.0 | Lenguaje de shaders · **HUMANO** | fase: F14, decisiones P15 (que se haga) y P16 | TODO |  |
| F15.1 | CSIR y su ensamblador | F15.0 | TODO |  |
| F15.2 | Intérprete en software | F15.1 | TODO |  |
| F15.3 | CSIR a SPIR-V y hardware | F15.2 | TODO |  |
| F15.4 | Compute | F15.3 | TODO |  |
| F15.5 | Documentación y ejemplos | F15.4 | TODO |  |

## F16 · Observabilidad y documentación (transversal)

Archivo: [phases/F16-observabilidad-docs.md](phases/F16-observabilidad-docs.md)

| ID | Tarea | Depende de | Estado | Commits |
| --- | --- | --- | --- | --- |
| F16.1 | Captura y repetición de fotogramas | F10 | TODO |  |
| F16.2 | Visores | F8 y F9 | TODO |  |
| F16.3 | Estadísticas por fotograma | F5 | TODO |  |
| F16.4 | Instantáneas por páginas | F4 | TODO |  |
| F16.5 | Doc 07 generado | F1.8 | TODO |  |
| F16.6 | Tutoriales y wiki | F5 (y se repite al cerrar cada hito) | TODO |  |

# Decisiones

Una decisión **CERRADA** es normativa y ya está reflejada en [SPEC.md](SPEC.md). Una **PENDIENTE** se cierra con
el usuario en la tarea que se indica; hasta entonces ninguna tarea que dependa de ella puede empezar. Cada
pendiente trae la opción por defecto recomendada: el agente la **propone**, no la aplica.

Al cerrar una decisión: pásala a la tabla de cerradas con la fecha y quién la tomó, actualiza SPEC.md y cita la
decisión en el commit.

## Cerradas

| ID | Decisión | Fecha | Origen |
| --- | --- | --- | --- |
| D01 | Arquitectura de GPU: propuesta C (híbrida por capas: VRAM mapeada, procesador de comandos, motores por nivel, ejecutor software y hardware) | 2026-09-25 | usuario |
| D02 | Sin retrocompatibilidad con la máquina v1 en dispositivos, mapa, IRQ, ABI ni cabeceras | 2026-09-25 | usuario |
| D03 | Todo se ve en la ventana SDL, incluido el nivel Terminal; el programa nunca usa el terminal del host, que queda para logs y depuración | 2026-09-25 | usuario |
| D04 | Terminal virtual y shell dibujados en la ventana | 2026-09-25 | usuario |
| D05 | Relojes fijos y configurables para CPU y GPU; tiempo virtual determinista; perfiles de máquina | 2026-09-25 | usuario |
| D06 | Resolución máxima 1920×1080, sólo con el perfil `custom` | 2026-09-25 | usuario |
| D07 | 64 bits nativos mediante pares de registros; la máquina sigue siendo de 32 bits | 2026-09-25 | usuario |
| D08 | RAM hasta 2 GiB en `0x00000000–0x7FFFFFFF`; VRAM hasta 1 GiB en `0xA0000000–0xDFFFFFFF`; MMIO en `0xFF000000–0xFFFFFFFF`; el resto vacío | 2026-09-25 | usuario |
| D09 | Varios niveles 2D: Retro (el más sencillo), Arcade y Vectorial; siete niveles de vídeo V0–V6 | 2026-09-25 | usuario (propuesta aceptada) |
| D10 | Cinco niveles de audio: A0 Tono MIDI, A1 PSG, A2 FM + MIDI, A3 Sampler, A4 Audio digital | 2026-09-25 | usuario (propuesta aceptada) |
| D11 | Registros de dispositivo sólo de 32 bits; `IODevice` sólo con `read`/`write` de 32 bits | 2026-09-25 | usuario |
| D12 | Un dispositivo por archivo, con su `.h` y su `.cpp`, agrupados por función | 2026-09-25 | usuario |
| D13 | Instrucciones: se mantienen desplazamientos y máscaras con una tabla `Field<>`; se descarta la unión con bitfields; conversión bytes↔`u32` explícita en little-endian | 2026-09-25 | evaluación aceptada |
| D14 | Ocho perfiles: micro, pocket, retro, arcade, polygon, standard (por defecto), workstation, custom | 2026-09-25 | usuario (propuesta aceptada) |
| D15 | Perfiles: los ocho de SPEC §4 tal cual, `custom` con CPU hasta 400 MHz (por encima de unos 150 MHz no llega a tiempo real en el host de desarrollo; sigue siendo determinista) (P01) | 2026-09-25 | usuario (opción por defecto, F0.5) |
| D16 | Refresco configurable de 50 o 60 Hz, 60 por defecto (P03) | 2026-09-25 | usuario (opción por defecto, F0.5) |
| D17 | Límite de sprites por línea en todos los perfiles; VRAM escribible sólo en el VBlank en `micro` y `pocket` (P04) | 2026-09-25 | usuario (opción por defecto, F0.5) |
| D18 | Un acceso de menos de 32 bits a MMIO es un fallo (`MmioWidth`) (P05) | 2026-09-25 | usuario (opción por defecto, F0.5) |
| D19 | Un offset MMIO no declarado lee 0 y descarta la escritura; con `--strict-mmio` es un fallo (P06) | 2026-09-25 | usuario (opción por defecto, F0.5) |
| D20 | Celda de texto de 16 bits por defecto (carácter de 8 bits, tinta y fondo de 4); la de 32 bits, opcional (P07) | 2026-09-25 | usuario (opción por defecto, F0.5) |
| D21 | `stderr` sólo en el terminal virtual, con el color de error; nunca se copia al host (P08) | 2026-09-25 | usuario (opción por defecto, F0.5) |
| D22 | La ventana queda abierta al terminar el programa hasta una tecla; `--exit-on-halt` la cierra (P09) | 2026-09-25 | usuario (opción por defecto, F0.5) |
| D23 | `double` es binary64 por defecto en Ceres-C; `-fshort-double` lo hace `float` (P10) | 2026-09-25 | usuario (opción por defecto, F0.5) |
| D24 | Valores de 64 bits en la ABI en pares alineados a registro par (P11) | 2026-09-25 | usuario (opción por defecto, F0.5) |
| D25 | Bytes de cada tecla en el terminal: la tabla actual de la VM (`ESC[A`…`ESC[6~`), fijada en SPEC §8.3 (surgió en F0.4) | 2026-09-25 | usuario (opción por defecto, F0.5) |

## Pendientes

| ID | Pregunta | Por defecto | Se cierra en | Bloquea |
| --- | --- | --- | --- | --- |
| P02 | Tabla de ciclos: la de SPEC §3.2 calibrada, o una más simple (todo 1 salvo memoria y división) | La de SPEC §3.2, calibrada con las medidas de F0.1 | F0.6 | F2.1 |
| P12 | FM de A2: 4 operadores (tipo OPN) o 2 (tipo OPL2) | 4 operadores | F9.0 | F9.4 |
| P13 | Códecs de A4: IMA-ADPCM y QOA, u otro | IMA-ADPCM y QOA | F11.0 | F11.4 |
| P14 | Banco General MIDI de muestras para A3: cuál y con qué licencia (se verifica antes de incluirlo) | Uno con licencia libre verificada (candidato: FluidR3_GM) | F11.0 | F11.3 |
| P15 | Alcance final: ¿se hace V6 (shaders)? ¿orden entre F10 (V3) y F12 (hardware)? | V6 opcional al final; F10 antes que F12 | antes de F12 | F12, F15 |
| P16 | Lenguaje de shaders de V6: bytecode ensamblado (CSIR) o un subconjunto de C | CSIR ensamblado | F15.0 | F15 |

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

## Pendientes

| ID | Pregunta | Por defecto | Se cierra en | Bloquea |
| --- | --- | --- | --- | --- |
| P01 | Valores exactos de los perfiles (tabla de SPEC §4) | Los de SPEC §4 | F0.5 | F4.5 |
| P02 | Tabla de ciclos: la de SPEC §3.2 calibrada, o una más simple (todo 1 salvo memoria y división) | La de SPEC §3.2, calibrada con las medidas de F0.1 | F0.6 | F2.1 |
| P03 | Refresco: 60 Hz fijo o 50/60 configurable | 50/60 configurable, 60 por defecto | F0.5 | F5.2 |
| P04 | Límites retro estrictos (sprites por línea; en `micro` y `pocket`, VRAM sólo escribible durante el VBlank) | Sprites por línea siempre; VRAM en VBlank sólo en `micro` y `pocket` | F0.5 | F8.3 |
| P05 | Accesos de menos de 32 bits a MMIO: fallo, o leer la palabra y extraer | Fallo (`MmioWidth`) | F0.5 | F1.6, F1.7 |
| P06 | Offsets de MMIO no declarados: leer 0 (fallo sólo con `--strict-mmio`), o fallo siempre | Leer 0 y fallo con `--strict-mmio` | F0.5 | F1.8 |
| P07 | Formato de celda por defecto del terminal: 16 o 32 bits | 16 bits | F0.5 | F5.3 |
| P08 | `stderr`: sólo en pantalla, o también copiado al log del host | Sólo en pantalla | F0.5 | F5.6 |
| P09 | Ventana al terminar el programa: abierta hasta una tecla, o cerrar | Abierta; `--exit-on-halt` la cierra | F0.5 | F5.5 |
| P10 | `double` binary64 por defecto con `-fshort-double`, o float por defecto | binary64 por defecto | F0.5 | F6.3 |
| P11 | Pares en la ABI alineados a registro par, o consecutivos como hoy | Alineados | F0.5 | F6.1 |
| P12 | FM de A2: 4 operadores (tipo OPN) o 2 (tipo OPL2) | 4 operadores | F9.0 | F9.4 |
| P13 | Códecs de A4: IMA-ADPCM y QOA, u otro | IMA-ADPCM y QOA | F11.0 | F11.4 |
| P14 | Banco General MIDI de muestras para A3: cuál y con qué licencia (se verifica antes de incluirlo) | Uno con licencia libre verificada (candidato: FluidR3_GM) | F11.0 | F11.3 |
| P15 | Alcance final: ¿se hace V6 (shaders)? ¿orden entre F10 (V3) y F12 (hardware)? | V6 opcional al final; F10 antes que F12 | antes de F12 | F12, F15 |
| P16 | Lenguaje de shaders de V6: bytecode ensamblado (CSIR) o un subconjunto de C | CSIR ensamblado | F15.0 | F15 |

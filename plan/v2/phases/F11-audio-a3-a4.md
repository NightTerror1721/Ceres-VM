# F11 · Audio A3–A4: sampler y audio digital

- **Tamaño**: L · **Depende de**: F9 · **Vía**: audio · **Repos**: CeresASM, STDLIB
- **Objetivo**: voces que reproducen muestras con bancos General MIDI y efectos (A3), y flujos PCM con
  decodificación y cadena maestra (A4). Con F10 forma el **hito 5**.
- **SPEC**: §9 (A3, A4); fija los registros de `0x21` (voces A3) y `0x23` (flujos y efectos).

## Tareas

### F11.0 · Códecs y banco de muestras · `HUMANO`
- **Decisiones**: P13, P14. Verifica la licencia del banco antes de proponerlo.
- **Commit**: `Close the codec and sample bank decisions (F11.0)`

### F11.1 · Voces del sampler
- **Depende de**: F9
- **Pasos**: 32 voces PCM de 8 o 16 bits o IMA-ADPCM leídas de RAM; tono en 16.16; bucle; ADSR; volumen y pan;
  filtro paso bajo por voz; IRQ 28 al terminar.
- **Commit**: `Add the A3 sampler voices (F11.1)`

### F11.2 · Efectos del sampler
- **Pasos**: eco, reverb y chorus globales con envíos por voz.
- **Commit**: `Add echo, reverb and chorus (F11.2)`

### F11.3 · Bancos de instrumentos
- **Depende de**: F11.1, F11.0 (P14)
- **Pasos**: formato `.cbank` (muestras, zonas por nota y velocidad, envolventes); el secuenciador de F9.5 usa un
  banco en lugar de FM por canal; herramienta `tools/sf2bank.js` (SoundFont → `.cbank`); el banco elegido en P14.
- **Commit**: `Add instrument banks for the sequencer (F11.3)`

### F11.4 · Flujos PCM
- **Depende de**: F11.1, F11.0 (P13)
- **Pasos**: 8 flujos (8/16 bits, mono/estéreo, 8–48 kHz) con búfer circular en RAM, `ReadPos`, `WritePos`,
  umbral e IRQ 30; remuestreo a 48 kHz; decodificador IMA-ADPCM y QOA (o lo que diga P13); EQ de 3 bandas,
  compresor/limitador y reverb en la cadena maestra.
- **Aceptación**: [ ] Un WAV de referencia reproducido y capturado con `--audio-out` coincide (tras el remuestreo documentado).
- **Commit**: `Add the A4 digital audio streams (F11.4)`

### F11.5 · STDLIB
- **Repos**: STDLIB · **Depende de**: F11.3, F11.4
- **Pasos**: `sampler.h`, reproductor MOD/XM, `pcm.h`, `sound.h` (efectos con prioridad), `wav.h`, `qoa.h`;
  `play` del shell con WAV y QOA; tests por hash.
- **Commit**: `Add the A3–A4 audio headers (F11.5)`

### F11.6 · Documentación
- **Commit**: `Document audio levels A3 and A4 (F11.6)`

## Notas

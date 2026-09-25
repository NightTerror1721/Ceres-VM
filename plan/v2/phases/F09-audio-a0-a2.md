# F9 · Audio A0–A2: tono, PSG, FM y secuenciador MIDI

- **Tamaño**: L · **Depende de**: F5 (y F2 para el tiempo virtual) · **Vía**: audio · **Repos**: CeresASM, STDLIB
- **Objetivo**: convertir el audio actual en niveles, en tiempo virtual y determinista, y añadir FM y un
  secuenciador MIDI. Con F8 forma el **hito 4** (consola retro).
- **SPEC**: §9, §5.5 (slots `0x20–0x23`), §5.6 (IRQ 28–30).

## Punto de partida

- `audio/audio.{h,cpp}` (antes `audio_device.h`): un tono (frecuencia, duración, volumen, onda) y 4 canales con
  onda, ciclo de trabajo y ADSR; `renderChannels` lo llama el hilo de audio del host (no determinista).
- STDLIB: `include/ceres/audio.h`, `include/ceres/music.h`, ejemplo `examples/tune.c`.

## Antes de empezar

Fija en SPEC §9 la disposición de registros de los slots `0x20` y `0x21` (A0–A2) y `0x22` (secuenciador) y el
formato de la secuencia en RAM (eventos con delta en ticks). Commit `plan: fix the A0–A2 register layout`.

## Tareas

### F9.0 · Decidir la FM · `HUMANO`
- **Decisiones**: P12. **Commit**: `Close the FM decision (F9.0)`

### F9.1 · Esqueleto del audio en tiempo virtual
- **Depende de**: F5, F2
- **Pasos**: slot `0x20` (control, nivel, capacidades, mezclador); el mezclador genera bloques de muestras en
  eventos del planificador (una muestra estéreo cada `CpuClockHz/48000` ciclos); `SdlAudio` consume un búfer
  elástico con remuestreo ligero; sin ventana, `--audio-out <wav>`.
- **Aceptación**: [ ] Dos ejecuciones dan el mismo WAV bit a bit, a cualquier `--speed`.
- **Commit**: `Mix audio on virtual time (F9.1)`

### F9.2 · A0 Tono con cola de notas
- **Pasos**: el tono actual con nota MIDI, velocidad y duración en ms virtuales; cola de 64 notas; IRQ 28 al vaciarse.
- **Commit**: `Add the A0 tone level with a note queue (F9.2)`

### F9.3 · A1 PSG
- **Pasos**: sustituye los 4 canales por las 4 voces de SPEC §9 (A1); envolventes; pan L/R.
- **Commit**: `Replace the four channels with the A1 PSG (F9.3)`

### F9.4 · A2 FM · requiere P12
- **Depende de**: F9.3, F9.0
- **Pasos**: 8 voces FM (4 u 2 operadores según P12), algoritmos, realimentación, ADSR por operador, LFO.
- **Aceptación**: [ ] WAV de referencia por algoritmo (hash).
- **Commit**: `Add the A2 FM synthesizer (F9.4)`

### F9.5 · Secuenciador MIDI y banco FM
- **Pasos**: slot `0x22`; eventos MIDI en RAM (nota, programa, volumen, pan, pitch bend, tempo, marcador) con
  tiempos en ticks; 16 canales; banco FM de estilo General MIDI incorporado (propio, sin licencias ajenas); IRQ 29.
- **Commit**: `Add the MIDI sequencer and the built-in FM bank (F9.5)`

### F9.6 · STDLIB y herramienta
- **Repos**: STDLIB
- **Pasos**: `tone.h`, `music.h` (sobre la cola), `psg.h`, `fm.h`, `midi.h`; `tools/midi2seq.js` (archivo MIDI
  estándar → secuencia); tests por hash del WAV; `play` en el shell para MIDI.
- **Commit**: `Add the A0–A2 audio headers and midi2seq (F9.6)`

### F9.7 · Documentación
- **Archivos**: crear `docs/32-Audio.md` (A0–A2).
- **Commit**: `Document audio levels A0 to A2 (F9.7)`

## Notas

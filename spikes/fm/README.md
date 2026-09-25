# Spike de síntesis FM (F0.4)

Prototipo aparte, fuera de `Ceres/` y del build normal. Una voz FM de 4 operadores como la del nivel A2 de la SPEC
§9, para medir cuánto cuesta cada muestra y comprobar que sale igual en cualquier host.

```bash
cmake -S spikes/fm -B spikes/fm/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build spikes/fm/build
spikes/fm/build/fm [--wav fm_note.wav] [--algorithm 0-7] [--seconds 10]
```

1. Escribe una nota (La4, 1,5 s pulsada y 0,5 s soltada) en un WAV de 48 kHz, 16 bits y mono, e imprime el hash
   FNV-1a de sus muestras.
2. Mide 8 voces con los 8 algoritmos a la vez, con un acorde que cambia cada medio segundo, e imprime el coste por
   muestra de voz y el hash de todo lo generado.

## Cómo está hecho

- **Sólo aritmética entera en tiempo de nota**: fase de 32 bits por operador, tabla de seno de 4 096 entradas en
  Q14, atenuación en pasos de 3/32 dB con su tabla de ganancia, envolvente ADSR lineal en dB (el ataque es
  exponencial, como en los chips FM), frecuencias desde una tabla de la octava 4 en milihercios.
- **8 algoritmos** clásicos de 4 operadores (del `1>2>3>4` en serie a los cuatro en paralelo) y **realimentación**
  en el operador 1 (media de las dos últimas salidas).
- Las dos tablas se calculan al arrancar con `std::sin` y `std::exp2`, redondeadas a enteros. Si dos bibliotecas
  matemáticas redondeasen distinto algún valor, el hash lo delataría.
- No lleva LFO ni desafinado (*detune*): no cambian el coste de forma apreciable y quedan para F9.4.

## Resultado (Ryzen 7 9800X3D)

| Compilador | Nota (hash) | 8 voces × 4 operadores | Uso de un núcleo a 48 kHz | Hash de las 8 voces |
| --- | --- | ---: | ---: | --- |
| GCC 15.2 (MSYS2), Release | `34260d418ff0c253` | 4,5 ns por muestra de voz | 0,17 % | `94bd935037c627aa` |
| MSVC 14.50, Release | `34260d418ff0c253` | 6,5 ns por muestra de voz | 0,25 % | `94bd935037c627aa` |

- **Determinista entre compiladores**: el mismo hash y el mismo WAV byte a byte con GCC y con MSVC, y el mismo en
  ejecuciones repetidas.
- **El coste no es un problema**: el nivel A2 completo (8 voces FM) cuesta menos del 0,3 % de un núcleo. El
  mezclador, los efectos y el remuestreo de A3–A4 pesarán más que la FM.
- El WAV de la nota tiene pico 17 696 y RMS 2 637 (de 32 767): suena y no satura.

**Recomendación para F9.4.** Mantener la síntesis en enteros y **meter las tablas en el código** (generadas una
vez por un script y comprobadas con un test de hash), en vez de calcularlas al arrancar con `std::sin`. Aquí han
coincidido GCC y MSVC, pero nada garantiza que otra libm redondee igual. El WAV de referencia por algoritmo de la
aceptación de F9.4 sale directamente de este esquema.

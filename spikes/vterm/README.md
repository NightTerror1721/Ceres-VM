# Spike del terminal virtual (F0.4)

Prototipo aparte, fuera de `Ceres/` y del build normal. Interpreta la salida real de programas Ceres con el
subconjunto ANSI de la SPEC §8.2 sobre una rejilla de celdas, y dice qué secuencias aparecen y si §8.2 las cubre.
Es el lado de salida del terminal virtual de la fase F5.

```bash
cmake -S spikes/vterm -B spikes/vterm/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build spikes/vterm/build
spikes/vterm/build/vterm [--cols 80] [--rows 25] [--screen] <archivo>...
```

Cada archivo es un flujo de bytes escrito al terminal. Imprime cada secuencia distinta con su número de apariciones
y `ok` o `MISSING`; con `--screen` vuelca además la pantalla final. Termina con 1 si falta alguna.

## Qué implementa

UTF-8 (un byte inválido se dibuja como `?`); `\r`, `\n`, `\b`, `\t` a múltiplos de 8, `\a`; `ESC [` con `A`, `B`,
`C`, `D`, `H`, `f`, `J` 0–2, `K` 0–2, `s`, `u`, `r`, `?25l`, `?25h` y `m` (0, 1, 7, 22, 27, 30–37, 39, 40–47, 49,
90–97, 100–107, `38;5;n`, `48;5;n`); `ESC 7` y `ESC 8`. Lo demás se consume y se cuenta como `MISSING`
(`38;2;r;g;b` incluido).

## Resultado

**Entrada**: las 116 salidas esperadas de la STDLIB (`tests/expected/*.expected` y `examples/expected/*.expected`),
que son la salida real de cada programa, byte a byte, tal como la comparan los tests.

**Todo cubierto: ninguna secuencia fuera de §8.2 y ningún UTF-8 inválido.**

| Secuencia | Apariciones | De dónde sale |
| --- | ---: | --- |
| `\n` | 1 348 | todos |
| `SGR 40–47` / `30–37` / `90–97` | 237 / 217 / 22 | la VM, al volcar el framebuffer de texto al terminal (`FramebufferDevice`: `ESC[fg;bgm` y `ESC[0m`) |
| `SGR 0` | 195 | la VM (volcado de texto) y `ANSI_RESET` |
| `CSI H` | 4 | `ansi_goto`, `ANSI_HOME`, `ANSI_CLEAR` |
| `SGR 38;5;n`, `48;5;n` | 2, 1 | `ansi_fg256`, `ansi_bg256` |
| `SGR 1`, `CSI A/B/C/D`, `J 2`, `K 2`, `s`, `u`, `?25l`, `?25h` | 1 cada una | `ansi.h` (`test_debug_ansi`) |

- `tui.h` no emite ANSI: dibuja en el framebuffer de texto (`textfb`). El ANSI de `test_tui` y `test_textfb_color`
  lo pone la VM al volcar ese framebuffer al terminal del host, y **desaparece en v2**: el nivel V0 dibuja el texto
  directamente en la ventana.
- **Inventario estático** de lo que la STDLIB puede emitir (`include/ceres/ansi.h`, `src/ceres/ansi.c`, y ningún
  otro archivo de `src/`): `A`, `B`, `C`, `D` con cuenta; `H` con fila y columna; `2J`; `2K`; `s`; `u`; `?25l`;
  `?25h`; `0m`, `1m`, `30–37m`, `40–47m`, `38;5;n`, `48;5;n`. Todo dentro de §8.2.
- `examples/rps_tui.c` usa `tui.h` pero no está en el repositorio (trabajo en curso) y no se ha medido.

## Lado de entrada (para F5 y §8.3)

La VM traduce las teclas de la ventana a bytes para el terminal (`keystrokeToTerminalBytes`,
`libs/devices/include/ceres/devices/input_devices.h`): `ESC` solo, `ESC[A`/`B`/`C`/`D` (flechas), `ESC[H` y
`ESC[F` (Inicio y Fin), `ESC[2~`, `ESC[3~`, `ESC[5~` y `ESC[6~` (Insertar, Suprimir, RePág, AvPág). El decodificador
de teclas de la STDLIB (`key.h`) y `tests/expected/test_key.stdin` dependen de ellas. **§8.3 no dice qué bytes
genera cada tecla en modo raw**: propuesta para F0.5, fijar en §8.3 exactamente esta tabla.

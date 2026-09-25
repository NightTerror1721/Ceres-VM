# Medidas de partida

El punto de partida medido antes de tocar la máquina. Las fases posteriores comparan contra estos números
(puertas de rendimiento de F2 y F4). Cada sección dice cómo repetir la medida.

## Host

| | |
| --- | --- |
| CPU | AMD Ryzen 7 9800X3D (8 núcleos, 4,7 GHz nominales) |
| SO | Windows 11 Pro, compilación 26200 |
| Compilador | GCC 15.2.0 (MSYS2 Rev8), preset `gcc-ipo` (LTO), configuración Release |
| Commit medido | CeresASM `d36d0f8` + el benchmark de F0.1 |

## Intérprete por clase de instrucción (F0.1)

```bash
cmake --build "D:/Projects/CeresASM/Ceres/build/gcc-ipo" --target ceres_vm_benchmarks --config Release
"D:/Projects/CeresASM/Ceres/build/gcc-ipo/bin/Release/ceres_vm_benchmarks.exe"            # todas
"D:/Projects/CeresASM/Ceres/build/gcc-ipo/bin/Release/ceres_vm_benchmarks.exe" mmio mixed  # sólo esas (y "loop")
```

**Método.** Cada benchmark es un bucle de 50 000 vueltas con 16 instrucciones de la clase (o las que se indique)
y la cola del bucle (`SUBI`, `CMPI`, `JNZ`). El ejecutable llama a `ExecutionEngine::step()` como lo hace
`CeresVM::run()`, sin dispositivos que avancen y sin ventana. Da la mediana de 7 muestras. Se ejecutó 3 veces;
la tabla recoge la mediana de las 3. «ns por instrucción de la clase» descuenta la cola del bucle, que se mide con
`loop` (un cuerpo vacío). El ruido entre ejecuciones es de un ±8 %.

| Benchmark | Qué ejecuta | MIPS | ns/instr. | ns por instr. de la clase |
| --- | --- | ---: | ---: | ---: |
| `loop` | sólo la cola del bucle | 174,9 | 5,72 | — |
| `alu` | `add`, `xor`, `rol`, `shli`, `and`, `or`, `sub`, `mov` | 140,5 | 7,12 | 7,39 |
| `ram` | `ldr`/`str` de 32, 16 y 8 bits en RAM | 108,4 | 9,23 | 9,88 |
| `mmio` | `ldr`/`str` de 32 bits a un dispositivo trivial (slot 1) | 87,7 | 11,41 | 12,47 |
| `branch` | `cmp` + 15 saltos (`jp`, `jz` no tomado, `jnz`, `jls`, `jle`) | 157,4 | 6,35 | 6,90 |
| `call-ret` | 8 × (`call` + `ret`) | 99,6 | 10,04 | 10,84 |
| `push-pop` | `push`, `push`, `pop`, `pop` | 107,9 | 9,27 | 9,94 |
| `pushm-popm` | `pushm`/`popm` de 4 registros | 39,9 | 25,06 | 32,30 |
| `mul` | `mul`, `imul`, `mulh`, `imulh` | 156,8 | 6,38 | 6,51 |
| `div` | `div`, `idiv`, `mod`, `imod` | 151,4 | 6,60 | 6,77 |
| `float` | `fadd`, `fmul`, `fsub`, `fmin`, `fabs`, `fneg`, `fcmp`, `itof` | 144,3 | 6,93 | 7,16 |
| `fdiv` | `fdiv`, `fsqrt` | 154,4 | 6,48 | 6,62 |
| `fma` | `fma` | 154,5 | 6,47 | 6,61 |
| `block-256` | `mcpy` y `mset` de 256 bytes, con 3 instrucciones de preparación cada uno | 121,6 | 8,22 | 34,75 ¹ |
| **`mixed`** | bucle tipo C sobre un array + llamada a una función con marco (23 instr./vuelta) | **134,3** | **7,45** | — |

¹ Incluye las 3 instrucciones de preparación (unos 20 ns); el `mcpy`/`mset` de 256 bytes en sí cuesta unos 15 ns.

**Lectura.**

- **Domina el despacho, no la operación.** `mul`, `div`, float, `fdiv` y `fma` cuestan lo mismo que la ALU
  (6,5–7,4 ns). La tabla de ciclos (SPEC §3.2) no tiene que imitar el coste en el host: es el coste de la máquina
  simulada.
- **Lo que toca memoria cuesta de 1,3 a 1,7 veces más.** Una carga o un almacenamiento en RAM cuesta unos 10 ns; un
  acceso MMIO, unos 12,5 ns (el bus despacha por slot con una llamada virtual); `call`/`ret` y `push`/`pop`, unos
  10–11 ns.
- **`pushm`/`popm` es lo más caro por instrucción**: unos 8 ns por registro.
- **Margen para el tiempo real (dato para F0.6 y P02).** Con la tabla provisional, el `mixed` gasta 37 ciclos en 23
  instrucciones (1,61 ciclos por instrucción). A 134 MIPS sostendría una CPU de unos 216 MHz al 100 % del host, o
  unos 150 MHz con el límite del 70 %:

  | Perfil | CPU | MIPS que pide el `mixed` | Uso del host |
  | --- | ---: | ---: | ---: |
  | `retro` | 16 MHz | 9,9 | 7 % |
  | `arcade` | 25 MHz | 15,5 | 12 % |
  | `standard` | 50 MHz | 31,1 | 23 % |
  | `workstation` | 100 MHz | 62,1 | 46 % |
  | `custom` al máximo | 400 MHz | 248 | 185 %: no llega en tiempo real |

  El peor caso es código de sólo ALU (1 ciclo por instrucción, unos 140 MIPS). `standard` pide 50 MIPS (36 %), y
  `workstation` pide 100 MIPS: un 71 %, al borde del límite. Estas cifras no cuentan el planificador de eventos
  (F2), la presentación ni los dispositivos, que se miden en F0.2.

## Presentación (F0.2)

**Programa.** Configura el display (slot 7) a W×H, presenta una vez (`CommandRegister` = 2) y hace 100 millones
de vueltas de `sub`/`ifne`: 300 000 016 instrucciones (contadas con `ceres profile`). Sin la configuración del
display sale la variante `compute`.

```casm
@text
global main:
    la   r7, 0xFFFF0000
    la   r6, 0xFF070000
    la   r1, 320            // W
    str  [r6 + 4], r1
    la   r1, 240            // H
    str  [r6 + 8], r1
    li   r1, 2
    str  [r6 + 0], r1       // present: desde aquí la ventana dibuja el display
    la   r2, 100000000
.loop:
    sub  r2, r2, 1
    ifne r2, 0, .loop
    li   r0, 1
    strb [r7 + 0], r0       // apagar
```

**Método.** `ceres.exe` con SDL (preset `gcc-sdl-ipo`, Release). Sin ventana: `CERES_HEADLESS=1 ceres run
<programa>`. Con ventana: `ceres run <programa>`, que abre la ventana en el primer `present`. Para separar el
tiempo se puso un contador temporal (no subido) en el bucle con ventana de `runMachine`
(`libs/driver/src/machine_runner.cpp`), que suma `steady_clock` alrededor de `pump`, del tramo de instrucciones y
de `present`, y lo imprime por stderr al terminar. Mediana de 3 ejecuciones.

> El ejecutable con SDL no entrega su stderr a PowerShell: captúralo desde Bash (`2> archivo`). Y el tiempo de
> reloj de las ejecuciones con ventana varía mucho según el estado del escritorio (entre 11 y 33 s para la misma
> variante de 320×240); por eso las cifras salen del contador y no de cronometrar el proceso.

| Variante | Tiempo total | Tramos (= `present`) | `present`/s | µs por `present` | µs por `pump` | MIPS efectivos | Pérdida |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Sin ventana (`CERES_HEADLESS=1`) | 2,80 s | — | — | — | — | **107,3** | — |
| Ventana, display 320×240 | 11,50 s | 73 243 | 6 371 | 96 | 1,2 | **26,1** | −76 % |
| Ventana, display 320×200 (`--window` sin programa de display) | 11,05 s | 73 243 | 6 630 | 94 | 1,3 | 27,2 | −75 % |
| Ventana, display 1280×720 | 46,49 s | 73 243 | 1 575 | 590 | 2,4 | **6,5** | −94 % |

**Lectura.**

- **La ventana presenta cada 4096 instrucciones, no a la frecuencia de la pantalla.** No hay vsync
  (`SDL_CreateRenderer` sin `SDL_SetRenderVSync`) y `HostBackend::instructionsPerFrame()` es 4096, así que a
  320×240 se presentan unas 6 400 imágenes por segundo, y cada una sube la textura entera y llama a
  `SDL_RenderPresent`. Eso se come el 61 % del tiempo a 320×240 y el 93 % a 1280×720.
- **`pump` no cuesta casi nada** (1–2 µs por llamada): el problema es sólo `present`.
- **El tramo de instrucciones en sí va más lento con ventana**: 73 MIPS a 320×240 y 97 a 1280×720, frente a 107.
  El bucle con ventana comprueba `isPoweredOn()` e `isHalted()` en cada paso y comparte la caché con SDL.
- **Qué significa para v2.** Presentando sólo en el vblank (60 veces por segundo, planificado por el reloj
  virtual de F2/F5), el coste medido sería de 60 × 96 µs ≈ 0,6 % del host a 320×240 y de 60 × 590 µs ≈ 3,5 % a
  1280×720. A 1920×1080, escalando por píxeles, unos 1,3 ms por imagen: un 8 % a 60 Hz. Es el argumento para
  presentar por vblank y subir sólo lo que ha cambiado (páginas sucias de VRAM).

## Memoria (F0.2)

**Método.** `CERES_HEADLESS=1 ceres run --memory <bytes> <programa>`. El arranque es la mediana de 5 ejecuciones
de un programa de una vuelta (proceso completo: cargar, ensamblar, ejecutar, salir). La memoria se lee del
proceso (PowerShell `Get-Process`: `WorkingSet64`, `PrivateMemorySize64`, `PeakWorkingSet64`) a los 1,5 s de un
programa que hace 150 millones de vueltas sin tocar memoria.

| `--memory` | Arranque (mediana) | Residente (WS) | Privada | Pico WS |
| ---: | ---: | ---: | ---: | ---: |
| 16 MiB (por defecto) | 14 ms | 25 MiB | 18 MiB | 25 MiB |
| 256 MiB | 44 ms | 265 MiB | 259 MiB | 265 MiB |
| 1 GiB (el máximo actual) | 138 ms | 1 033 MiB | 1 028 MiB | 1 033 MiB |

**Lectura.**

- **La RAM de la máquina se reserva y se pone a cero entera al arrancar** (`Memory` guarda un `std::vector<u8>`
  del tamaño pedido): el proceso ocupa la RAM configurada más unos 9 MiB aunque el programa no toque nada, y el
  arranque crece unos 0,12 ms por MiB.
- **Proyección para v2 sin reserva perezosa**: 2 GiB de RAM + 1 GiB de VRAM serían unos 3 GiB residentes y unos
  400 ms de arranque para cualquier programa. Con reserva perezosa (`VirtualAlloc`/`mmap`, F4) el coste pasa a
  ser proporcional a lo que el programa usa. La puerta de F4 compara contra esta tabla.

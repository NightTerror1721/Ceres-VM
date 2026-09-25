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

Pendiente.

## Memoria (F0.2)

Pendiente.

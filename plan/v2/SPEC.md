# Especificación de la Máquina Ceres v2

Fuente única de números para el refactor. Cada sección tiene un estado:

- **NORMATIVA**: se implementa tal cual. Cambiarla requiere confirmación del usuario (ver README, «Cuándo parar»).
- **PROVISIONAL**: se cierra en la fase indicada; quien la cierre la pasa a NORMATIVA en el mismo commit.

Convenciones: direcciones y offsets en hexadecimal; `KiB` = 1024 bytes; «ciclo» = ciclo de CPU salvo que se
diga otra cosa; los offsets de registro son relativos a la base del slot.

---

## 1. Invariantes (NORMATIVA)

1. La máquina es de **32 bits**: registros enteros y float de 32 bits, direcciones de 32 bits, palabra de 32
   bits, `size_t` y punteros de 32 bits. No existe ningún registro de 64 bits.
2. Los tipos de 64 bits son **operaciones sobre pares de registros** (sección 6).
3. Todo registro de dispositivo es de **32 bits** y sólo se accede con accesos de 32 bits alineados (sección 5).
4. El programa **nunca** lee ni escribe el terminal del host. Su E/S pasa por los dispositivos de la máquina.
5. El tiempo de la máquina es **virtual** y se deriva de sus relojes. Con la misma entrada (sellada en tiempo
   virtual) y la misma configuración, dos ejecuciones producen el mismo resultado bit a bit, en cualquier host
   y a cualquier `--speed`. La única fuente no determinista es el RTC, que se puede fijar.
6. El ejecutor software de la GPU es la **referencia** bit-exacta. El hardware no cambia nada observable salvo
   la velocidad real (y, en V4–V6, diferencias de antialiasing y precisión dentro de una tolerancia
   documentada que sólo ve quien lee la VRAM).
7. Sin retrocompatibilidad con la máquina v1: no se añaden capas de compatibilidad.

## 2. Mapa físico de memoria (NORMATIVA)

| Rango | Tamaño | Región | Accesos | Fuera de lo respaldado |
| --- | --- | --- | --- | --- |
| `0x00000000–0x7FFFFFFF` | hasta 2 GiB | RAM | 8, 16, 32, 64 bits (con las alineaciones de la ISA) | `MemoryFault` (motivo `OutOfRam`) |
| `0x80000000–0x9FFFFFFF` | 512 MiB | vacío | — | `MemoryFault` (motivo `Unmapped`) |
| `0xA0000000–0xDFFFFFFF` | hasta 1 GiB | VRAM | 8, 16, 32, 64 bits | `MemoryFault` (motivo `OutOfVram`) |
| `0xE0000000–0xFEFFFFFF` | 496 MiB | vacío | — | `MemoryFault` (motivo `Unmapped`) |
| `0xFF000000–0xFFFFFFFF` | 16 MiB | MMIO, 256 slots de 64 KiB | sólo 32 bits alineados | ver sección 5 |

- RAM respaldada: `[0, RamSize)`. VRAM respaldada: `[0xA0000000, 0xA0000000 + VramSize)`.
- Las regiones fijas de RAM no cambian: página nula `0x000–0x0FF`, BIOS `0x100–0x3FF`, programa desde `0x400`,
  pila del sistema de 4 KiB en lo alto de la RAM. `sp` arranca en `RamSize` (con 2 GiB, `0x80000000`).
- `RamSize`: de 8 KiB a 2 GiB, múltiplo de 4 KiB. `VramSize`: de 16 KiB a 1 GiB, múltiplo de 4 KiB.
- Enrutado: `p < 0xA0000000` → RAM (con su comprobación de tamaño); `p ≥ 0xA0000000` → VRAM, vacío o MMIO.
  El acceso a RAM hace **una sola comparación** extra como hoy.
- La MMU puede mapear marcos de VRAM y de MMIO en el espacio virtual (como hoy con MMIO).
- El host reserva RAM y VRAM con `VirtualAlloc` (Windows) o `mmap` (POSIX): páginas a cero bajo demanda.
  Un perfil de 2 GiB que toca 1 MiB no ocupa más de unos pocos MiB en el host.
- Instrucciones de bloque (`mcpy`, `mset`, `mcmp`, `mscan`): rápidas sobre RAM y VRAM; sobre MMIO, `MemoryFault`
  (motivo `MmioBlock`).

## 3. Relojes (NORMATIVA)

### 3.1 Dominios

| Reloj | Qué cuenta | Por defecto (`standard`) | Configuración |
| --- | --- | --- | --- |
| CPU | Ciclos de instrucción (tabla 3.2). Base maestra: contador `u64`. | 50 MHz | `--cpu-clock` |
| GPU | Ciclos de los motores de vídeo (coste por comando). | 200 MHz | `--gpu-clock` |
| Vídeo | Refresco: 50 o 60 Hz; líneas totales = alto visible + 45 de blanking (mínimo 262). | 60 Hz | `--refresh 50\|60` |
| Audio | Una muestra estéreo cada `CpuClockHz / 48000` ciclos. | 48 kHz | fijo |
| RTC | Segundos desde 1970: valor de arranque + tiempo virtual. | host al arrancar | `--rtc AAAA-MM-DDThh:mm:ss` |

Conversión entre dominios con razones enteras y resto acumulado (sin coma flotante, sin deriva).
Ejemplo: 50 MHz / 60 Hz = 833 333 ciclos por fotograma y resto 20; el resto se suma al siguiente.

### 3.2 Ciclos por instrucción (NORMATIVA desde F0.6, D26)

Calibrada con las medidas de F0.1 ([BASELINE.md](BASELINE.md)): con esta tabla, un programa mixto gasta 1,61 ciclos
por instrucción y `standard` (50 MHz) usa el 23 % del intérprete en el host de desarrollo; `workstation`, el 46 %.
Los costes modelan la máquina simulada, no el host (allí una división cuesta lo mismo que una suma).

| Clase | Ciclos |
| --- | --- |
| ALU de 32 bits, `mov`, `li`, `lui`, lógica, desplazamientos, `cmp`, bits (`clz`, `popcnt`…) | 1 |
| Carga/almacenamiento de 32 bits o menos en RAM | 2 |
| Carga/almacenamiento en VRAM | 3 |
| Acceso a MMIO | 4 |
| Salto condicional no tomado / tomado; `jp` | 1 / 2; 2 |
| `call`, `ret`, `bl`, `enter`, `leave` | 3 |
| `push`, `pop` | 2 |
| `pushm`, `popm`, `fpushm`, `fpopm` | 1 + 2 por registro |
| `mul`, `imul`, `mulh`, `imulh` | 3 |
| `div`, `idiv`, `mod`, `imod` | 16 |
| Float: `fadd`, `fsub`, `fmul`, `fcmp`, `fmin`, `fmax`, `fabs`, `fneg`, conversiones | 3 |
| Float: `fdiv`, `fsqrt`, `fmod`, `frecipe`, `frsqrte` | 12 |
| `fma` | 4 |
| Bloque (`mcpy`, `mset`, `mcmp`, `mscan`) | 4 + bytes/8 (redondeo hacia arriba) |
| Entrada a interrupción / `iret` | 12 / 6 |
| `halt` | el reloj salta al siguiente evento programado |
| Instrucciones de 64 bits | tabla 6.4 |

### 3.3 Planificador y ritmo

- Planificador de eventos: cada dispositivo programa su próximo evento en ciclos absolutos. El bucle de la CPU
  hace `cycles += cost[op]; if (cycles >= nextEvent) service();`. Sustituye a `IODevice::tick`,
  `advance`, `needsTick`, `ticksUntilEvent` y al reloj de halt (`setHaltClock`, `HaltClockRegister`).
- Ritmo respecto al host (`--speed`): `realtime` (por defecto con ventana) espera para que el tiempo virtual no
  adelante al real; `max` (por defecto sin ventana) no espera; `<factor>x` (`0.5x`, `2x`) escala el tiempo real.
- Entrada del host: se inyecta en cada milisegundo virtual y en cada VBlank, y cada evento queda sellado con su
  ciclo. `--record <archivo>` guarda la entrada sellada; `--replay <archivo>` la reproduce exacta.

## 4. Perfiles (NORMATIVA)

| Perfil | CPU | GPU | RAM | VRAM | Vídeo máx. | Resolución máx. | Audio máx. | Sprites/línea |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `micro` | 2 MHz | 2 MHz | 64 KiB | 32 KiB | V2 | 256×192 | A1 | 16 |
| `pocket` | 8 MHz | 8 MHz | 512 KiB | 96 KiB | V2 | 240×160 | A1 | 32 |
| `retro` | 16 MHz | 32 MHz | 2 MiB | 512 KiB | V2 | 320×240 | A2 | 32 |
| `arcade` | 25 MHz | 50 MHz | 8 MiB | 4 MiB | V3 | 640×480 | A3 | 96 |
| `polygon` | 33 MHz | 66 MHz | 16 MiB | 8 MiB | V5 | 640×480 | A3 | 96 |
| `standard` (por defecto) | 50 MHz | 200 MHz | 64 MiB | 32 MiB | V5 | 1280×720 | A4 | 128 |
| `workstation` | 100 MHz | 400 MHz | 512 MiB | 256 MiB | V6 | 1280×720 | A4 | 256 |
| `custom` | ≤ 400 MHz | ≤ 1 GHz | ≤ 2 GiB | ≤ 1 GiB | V6 | **1920×1080** | A4 | 256 |

- `--profile <nombre>` elige un perfil. Cualquier opción suelta (`--cpu-clock`, `--gpu-clock`, `--ram`, `--vram`,
  `--max-video`, `--max-audio`, `--max-resolution`) parte del perfil elegido y lo convierte en `custom`.
- **1920×1080 sólo es alcanzable con `custom`.** Ningún otro perfil pasa de 1280×720.
- Terminal máximo (celdas de 8×16): ancho/8 × alto/16 de la resolución máxima (32×12 en `micro`, 240×67 en `custom`).
- `SystemControl.ProfileId`: 0 micro, 1 pocket, 2 retro, 3 arcade, 4 polygon, 5 standard, 6 workstation, 7 custom.

## 5. Bus y dispositivos (NORMATIVA)

### 5.1 Reglas de acceso a MMIO

| Acceso | Resultado |
| --- | --- |
| `ldr`, `str`, `fldr`, `fstr` (y sus formas indexadas y relativas al PC) a un offset alineado a 4 | Un acceso de 32 bits a `IODevice::read`/`write` |
| `ldrd`, `strd`, `fldr.d`, `fstr.d` | Dos accesos de 32 bits: primero `offset`, después `offset + 4` |
| `ldrb`, `ldrh`, `ldrsb`, `ldrsh`, `strb`, `strh` | `MemoryFault`, motivo `MmioWidth` |
| Acceso desalineado | `AlignmentFault`, motivo `MmioWidth` |
| `mcpy`, `mset`, `mcmp`, `mscan` que toquen MMIO | `MemoryFault`, motivo `MmioBlock` |
| Slot sin dispositivo | Lectura `0xFFFFFFFF`, escritura ignorada (como hoy) |
| Offset no declarado en la tabla del dispositivo | Lectura 0, escritura ignorada; con `--strict-mmio`, `MemoryFault` motivo `MmioUndeclared` |

### 5.2 Interfaz de dispositivo

```cpp
// libs/vm/include/ceres/vm/io_device.h
class IODevice {
public:
    virtual ~IODevice() = default;
    virtual u32  read(Address offset) = 0;              // offset alineado a 4, relativo al slot
    virtual void write(Address offset, u32 value) = 0;
    virtual void reset() {}                             // SystemControl reset: vuelve al estado de arranque
    virtual void onEvent(u64 cycle) {}                  // el planificador llama cuando vence un evento suyo
    virtual const RegisterMap& registers() const = 0;   // tabla declarativa (5.3)
    virtual void saveState(StateWriter&) const {}
    virtual void loadState(StateReader&) {}
protected:
    Memory& memory(); Vram& vram(); Scheduler& scheduler();
    void raiseInterrupt(InterruptNumber);
};
```

Los componentes internos de un dispositivo (planos y motores de la GPU, voces del audio) **no** son `IODevice`.

### 5.3 Tabla de registros

Cada dispositivo expone una `RegisterMap` con, por registro: offset, nombre, acceso (`Read`, `Write`,
`ReadWrite`, `WriteOneToClear`), valor de reset y una frase de descripción. La usan: el comando `dev <nombre>`
del debugger, `--strict-mmio`, los tests y la generación de la documentación (doc 07). El despacho de `read`/
`write` sigue siendo un `switch` en el `.cpp`; la tabla no se consulta en el camino caliente.

### 5.4 Motivos de fallo (`SystemControl.FaultReason`)

| Código | Nombre | Cuándo |
| --- | --- | --- |
| 0 | `None` | — |
| 1 | `Alignment` | Acceso desalineado a RAM o VRAM |
| 2 | `OutOfRam` | Dirección por debajo de `0xA0000000` fuera de `RamSize` |
| 3 | `OutOfVram` | Dirección de VRAM fuera de `VramSize` |
| 4 | `Unmapped` | Regiones vacías del mapa |
| 5 | `MmioWidth` | Acceso a MMIO que no es de 32 bits o está desalineado |
| 6 | `MmioBlock` | Instrucción de bloque sobre MMIO |
| 7 | `MmioUndeclared` | Offset no declarado con `--strict-mmio` |
| 8 | `RegisterPair` | Par de registros inválido en una instrucción de 64 bits (`IllegalInstruction`) |
| 9 | `UnknownOpcode` | Opcode no asignado (`IllegalInstruction`) |
| 10 | `BadSubfield` | Subcampo no válido (p. ej. tipo de `fcvt` 14 o 15) (`IllegalInstruction`) |

### 5.5 Mapa de slots

| Slot | Base | Dispositivo | Archivo (en `libs/devices/include/ceres/devices/`) |
| --- | --- | --- | --- |
| `0x00` | `0xFF000000` | Terminal virtual | `terminal/terminal.h` |
| `0x01` | `0xFF010000` | Timer | `system/timer.h` |
| `0x02` | `0xFF020000` | DMA | `system/dma.h` |
| `0x03` | `0xFF030000` | Registro de depuración | `system/debug_log.h` |
| `0x10` | `0xFF100000` | Teclado | `input/keyboard.h` |
| `0x11` | `0xFF110000` | Ratón | `input/mouse.h` |
| `0x12` | `0xFF120000` | Gamepad | `input/gamepad.h` |
| `0x20–0x23` | `0xFF200000` | Audio (control, voces, secuenciador, flujos) | `audio/audio.h` |
| `0x30` | `0xFF300000` | Disco | `storage/disk.h` |
| `0x31` | `0xFF310000` | HostFs | `storage/host_fs.h` |
| `0x32` | `0xFF320000` | Periféricos (puertos y cartuchos) | `storage/peripherals.h` |
| `0x40–0x43` | `0xFF400000` | GPU (núcleo y pantalla, comandos, 2D/vectorial, 3D) | `video/gpu.h` |
| `0xFF` | `0xFFFF0000` | Control del sistema | `system/system_control.h` |

Los demás slots están reservados. Un dispositivo de varios slots se adjunta con `attachRange`.

### 5.6 Interrupciones

| IRQ | Fuente | IRQ | Fuente |
| --- | --- | --- | --- |
| 16 | Timer: cuenta atrás | 28 | Audio: fin de voz o de cola de notas |
| 17 | Timer: alarma | 29 | Audio: evento o marcador del secuenciador |
| 18 | DMA terminado | 30 | Audio: un flujo pide datos |
| 19 | Terminal: entrada disponible o Ctrl+C | 31 | reservada (audio) |
| 20 | Teclado | 32 | GPU: VBlank |
| 21 | Ratón | 33 | GPU: línea (`LineCompare`) |
| 22 | Gamepad | 34 | GPU: fence o motor de copia |
| 23 | reservada | 35 | GPU: fallo |
| 24 | Disco | 36–63 | reservadas |
| 25 | HostFs | | |
| 26 | Periféricos | | |
| 27 | reservada | | |

### 5.7 Registros de los dispositivos de sistema

**SystemControl (`0xFFFF0000`)** — los registros actuales se mantienen en sus offsets; se añaden:

| Offset | Nombre | Acceso | Descripción |
| --- | --- | --- | --- |
| `0x00` | Command | W | Byte bajo: 1 apagar, 2 reset, **3 cargar y ejecutar** (ver `LoadPath`); byte siguiente: código de salida |
| `0x04` | MemorySize | R | `RamSize` |
| `0x08` | Features | RW | Como hoy |
| `0x0C` | StackLimit | RW | Como hoy |
| `0x10` | FaultAddress | R | Como hoy |
| `0x14` | FaultAccess | R | Como hoy (bits 0–7 tipo de acceso, 8–31 tamaño) |
| `0x18–0x20` | ArgumentCount, ArgumentVector, Environment | R | Como hoy |
| `0x24` | CpuClockHz | R | Frecuencia de la CPU |
| `0x28` | ProfileId | R | Sección 4 |
| `0x2C` | FaultReason | R | Sección 5.4 |
| `0x30` | LoadPath | W | Dirección de una cadena con la ruta HostFs del `.cres` para el comando 3 |
| `0x34` | LoadArgs | W | Dirección de un bloque de argumentos (argc, argv) para el comando 3 |
| `0x38` | VramSize | R | `VramSize` (copia del de la GPU, para quien no tenga GPU) |

**Timer (`0xFF010000`)** — sustituye al actual:

| Offset | Nombre | Acceso | Descripción |
| --- | --- | --- | --- |
| `0x00` | CyclesLow | R | Ciclos de CPU desde el arranque, 32 bits bajos; fija `CyclesHigh` |
| `0x04` | CyclesHigh | R | 32 bits altos fijados por la última lectura de `CyclesLow` |
| `0x08` | Countdown | RW | Ciclos hasta la IRQ 16; 0 desarma; la lectura da lo que falta |
| `0x0C` | CountdownControl | RW | b0 periódico (se rearma con el último valor escrito) |
| `0x10` | NanosLow | R | Nanosegundos virtuales desde el arranque, bajos; fija `NanosHigh` |
| `0x14` | NanosHigh | R | Altos |
| `0x18` | Millis | R | Milisegundos virtuales (32 bits, se desborda) |
| `0x1C` | Rtc | R | Segundos desde 1970 (bajos) |
| `0x20` | AlarmLow | RW | Instante de alarma en nanosegundos virtuales, bajos |
| `0x24` | AlarmHigh | RW | Altos; escribirlo arma la alarma (0:0 desarma); IRQ 17 |
| `0x28` | CpuClockHz | R | Igual que en SystemControl |

**DMA (`0xFF020000`)**: registros actuales (Source `0x00`, Destination `0x04`, Length `0x08`, Command `0x0C`,
Status `0x10`, Transferred `0x14`). Fuente y destino pueden ser RAM o VRAM. Coste: 1 ciclo por cada 8 bytes,
se completa en su evento; IRQ 18.

**Registro de depuración (`0xFF030000`)**:

| Offset | Nombre | Acceso | Descripción |
| --- | --- | --- | --- |
| `0x00` | Output | W | Byte bajo: un carácter del log; `\n` emite la línea |
| `0x04` | Level | RW | 0 error, 1 aviso, 2 info, 3 depuración (prefijo de la línea) |
| `0x08` | Flush | W | Emite la línea pendiente |
| `0x0C` | Break | W | Bajo `ceres debug`, detiene la ejecución como un punto de ruptura; si no, nada |
| `0x10` | Enabled | R | 1 si el host recoge logs |

Salida en el host: `[ceres:<nivel>] <línea>` en stderr, o en el archivo de `--log`.

**Entrada y almacenamiento**: teclado, ratón, gamepad, disco, HostFs y periféricos conservan su disposición de
registros actual; sólo cambian de slot e IRQ, pierden los accesos de menos de 32 bits y sus eventos pasan por
el planificador.

## 6. ISA de 64 bits (NORMATIVA)

### 6.1 Formato

Sin cambios: `opcode` 31:24, `rd` 23:20, `rs` 19:16, `rt` 15:12, `imm16`/`simm16` 15:0, `imm8` 7:0. Las
instrucciones de tres registros usan los bits 11:0 para subcampos; las de dos registros, los bits 7:0.
Los campos se declaran con `Field<pos, width>` (sección 6.6).

### 6.2 Pares

- Enteros: `xN` = `r(2N):r(2N+1)`, N = 0..6. `x7` (`r14:r15`, `fp:sp`) está prohibido.
- Dobles: `dN` = `f(2N):f(2N+1)`, N = 0..7.
- La palabra baja va en el registro par (igual que en memoria, little-endian).
- Un campo de par con índice impar, o `x7`, provoca `IllegalInstruction` con `FaultReason = RegisterPair`.
- El banco float guarda los bits en crudo (`u32`), no como `float` del host.

### 6.3 Flags

- `add64`, `sub64`, `neg64`, `cmp64`: Z (resultado 0), S (bit 63), C (acarreo o préstamo sin signo de 64 bits),
  O (desbordamiento con signo de 64 bits).
- `mull`, `imull`, `mul64`: Z y S del resultado de 64 bits; C y O a 0.
- `div64`, `mod64`: Z, S; divisor 0 → trap como `div` (destino sin cambios). `idiv64`: `INT64_MIN / -1` pone O y
  da `INT64_MIN`.
- Desplazamientos: Z, S, C (último bit que sale); con cantidad 0, flags sin cambios (como los de 32 bits).
- Dobles: como sus equivalentes de 32 bits (`fcmp.d` como `fcmp`).
- Tras `cmp64` sirven los saltos existentes (`jz`, `jgr`, `jab`, `jls`, `jbl`…).

### 6.4 Opcodes

| Opcode | Mnemónico | Operandos | Codificación | Operación | Ciclos |
| --- | --- | --- | --- | --- | --- |
| `0xD7` | `add64` | `xd, xs, xt` | rd, rs, rt | `xd = xs + xt` | 2 |
| `0xD8` | `sub64` | `xd, xs, xt` | rd, rs, rt | `xd = xs - xt` | 2 |
| `0xD9` | `neg64` | `xd, xs` | rd, rs | `xd = -xs` | 2 |
| `0xDA` | `cmp64` | `xs, xt` | rs, rt | flags de `xs - xt` | 2 |
| `0xDB` | `mull` | `xd, rs, rt` | rd (par), rs, rt | `xd = (u64)rs * (u64)rt` | 4 |
| `0xDC` | `imull` | `xd, rs, rt` | rd (par), rs, rt | `xd = (i64)rs * (i64)rt` | 4 |
| `0xDD` | `mul64` | `xd, xs, xt` | rd, rs, rt | 64 bits bajos de `xs * xt` | 6 |
| `0xDE` | `div64` | `xd, xs, xt` | rd, rs, rt | cociente sin signo | 40 |
| `0xDF` | `idiv64` | `xd, xs, xt` | rd, rs, rt | cociente con signo | 40 |
| `0xE0` | `mod64` | `xd, xs, xt` | rd, rs, rt | resto sin signo | 40 |
| `0xE1` | `imod64` | `xd, xs, xt` | rd, rs, rt | resto con signo | 40 |
| `0xE2` | `shl64` | `xd, xs, rt` | rd, rs, rt (entero) | `xd = xs << (rt & 63)` | 2 |
| `0xE3` | `shr64` | `xd, xs, rt` | rd, rs, rt | lógico | 2 |
| `0xE4` | `sar64` | `xd, xs, rt` | rd, rs, rt | aritmético | 2 |
| `0xE5` | `shl64`/`shr64`/`sar64` | `xd, xs, imm6` | rd, rs; bits 7:6 tipo (0 shl, 1 shr, 2 sar; 3 inválido), 5:0 cantidad | por inmediato | 2 |
| `0xE6` | `sxt64` | `xd, rs` | rd (par), rs | `xd = (i64)(i32)rs` | 1 |
| `0xE7` | `clz64`/`ctz64`/`popcnt64` | `rd, xs` | rd (entero), rs (par); bits 1:0 op (0 clz, 1 ctz, 2 popcnt; 3 inválido) | bits | 2 |
| `0xE8` | `mov64` | `xd, xs` | rd, rs | copia | 1 |
| `0xE9` | `ldrd` | `xd, [rs + simm16]` | rd, rs, simm16 | carga de 64 bits, alineación 4 | 3 |
| `0xEA` | `strd` | `[rd + simm16], xs` | rd base, rs par, simm16 | almacenamiento de 64 bits | 3 |
| `0xEB` | `ldrd` | `xd, [rs + rt]` | rd, rs, rt | indexada | 3 |
| `0xEC` | `strd` | `[rd + rt], xs` | rd base, rt índice, rs par | indexada | 3 |
| `0xED` | `ldrd` | `xd, [pc + simm16]` | rd, simm16 | relativa al PC (misma base que `ldrp`) | 3 |
| `0xEE` | — | — | — | libre | — |
| `0xEF` | `fadd.d` | `dd, ds, dt` | rd, rs, rt | suma IEEE binary64 | 4 |
| `0xF0` | `fsub.d` | `dd, ds, dt` | rd, rs, rt | resta | 4 |
| `0xF1` | `fmul.d` | `dd, ds, dt` | rd, rs, rt | producto | 5 |
| `0xF2` | `fdiv.d` | `dd, ds, dt` | rd, rs, rt | división; divisor 0 → trap como `fdiv` (salvo `FeatureIeeeDivide`) | 20 |
| `0xF3` | `fma.d` | `dd, ds, dt` | rd, rs, rt | `dd = dd + ds * dt`, un redondeo | 6 |
| `0xF4` | `fsqrt.d` | `dd, ds` | rd, rs | raíz | 24 |
| `0xF5` | `fcmp.d` | `ds, dt` | rs, rt | flags como `fcmp` | 3 |
| `0xF6` | `fmin.d`/`fmax.d` | `dd, ds, dt` | rd, rs, rt; bit 0: 0 min, 1 max | | 3 |
| `0xF7` | `fmod.d` | `dd, ds, dt` | rd, rs, rt | resto IEEE; divisor 0 como `fmod` | 30 |
| `0xF8` | `fneg.d`, `fabs.d`, `fround.d`, `ffloor.d`, `fceil.d`, `ftrunc.d` | `dd, ds` | rd, rs; bits 2:0 op 0–5 (6, 7 inválidos) | unarias | 3 |
| `0xF9` | `fcopysign.d` | `dd, ds, dt` | rd, rs, rt | magnitud de `ds`, signo de `dt` | 2 |
| `0xFA` | `fclass.d` | `rd, ds` | rd (entero), rs (par) | máscara como `fclass` | 2 |
| `0xFB` | `fcvt.<a>.<b>` | según tipo | rd, rs; bits 3:0 tipo (tabla 6.5) | conversión | 3 (4 las de 64 bits enteros) |
| `0xFC` | `fmov.d` | `dd, ds` | rd, rs | copia | 1 |
| `0xFD` | `fldr.d` | `dd, [rs + simm16]` | rd, rs, simm16 | carga | 3 |
| `0xFE` | `fstr.d` | `[rd + simm16], ds` | rd base, rs par, simm16 | almacenamiento | 3 |
| `0x7A` | `fldr.d` | `dd, [rs + rt]` | rd, rs, rt | indexada | 3 |
| `0x7B` | `fstr.d` | `[rd + rt], ds` | rd base, rt índice, rs par | indexada | 3 |
| `0x7C` | `fldr.d` | `dd, [pc + simm16]` | rd, simm16 | relativa al PC | 3 |
| `0x7D` | `mtf.d` | `dd, xs` | rd, rs | bits de un par entero a un par doble | 1 |
| `0x7E` | `mff.d` | `xd, ds` | rd, rs | bits de un par doble a un par entero | 1 |

Libres tras esto: `0x0F`, `0x4F`, `0x7F`, `0x8C–0x8F`, `0xA4–0xB3`, `0xEE` (23 opcodes).

### 6.5 Tipos de `fcvt` (bits 3:0)

`w` = entero de 32 bits con signo, `wu` sin signo, `l` = par entero de 64 bits con signo, `lu` sin signo,
`s` = float, `d` = par doble. Destino ← origen.

| Tipo | Mnemónico | Destino ← Origen | Tipo | Mnemónico | Destino ← Origen |
| --- | --- | --- | --- | --- | --- |
| 0 | `fcvt.d.s` | dd ← fs | 7 | `fcvt.d.lu` | dd ← xs |
| 1 | `fcvt.s.d` | fd ← ds | 8 | `fcvt.l.d` | xd ← ds |
| 2 | `fcvt.d.w` | dd ← rs | 9 | `fcvt.lu.d` | xd ← ds |
| 3 | `fcvt.d.wu` | dd ← rs | 10 | `fcvt.s.l` | fd ← xs |
| 4 | `fcvt.w.d` | rd ← ds | 11 | `fcvt.s.lu` | fd ← xs |
| 5 | `fcvt.wu.d` | rd ← ds | 12 | `fcvt.l.s` | xd ← fs |
| 6 | `fcvt.d.l` | dd ← xs | 13 | `fcvt.lu.s` | xd ← fs |

A entero: truncan hacia cero y saturan (NaN → 0), como `ftoi`. A float: redondeo al par más cercano.

### 6.6 Codificación en el código (NORMATIVA)

- Se mantienen desplazamientos y máscaras sobre un `u32`. **No** se usa `union` con bitfields: leer un miembro
  inactivo es comportamiento indefinido en C++ y el orden de los bitfields depende de la implementación.
- Los campos se declaran una vez: `template <unsigned Pos, unsigned Width> struct Field { static constexpr u32
  get(u32); static constexpr u32 set(u32, u32); };` y `using Rd = Field<20, 4>;` etc. Accesores, ensamblador,
  desensamblador y documentación usan esa tabla.
- Bytes ↔ `u32` sólo en funciones explícitas `loadLittleEndian32`/`storeLittleEndian32` (con `std::endian` y
  `std::byteswap`). Nada de `reinterpret_cast` sobre el valor de una instrucción.

### 6.7 ABI (NORMATIVA desde F6)

- `long long` / `unsigned long long`: argumentos en `x0`, `x1` (pares alineados: con `(int, long long)` el entero
  va en `r0` y el par en `r2:r3`); retorno en `x0`. En la pila, 8 bytes con alineación 4.
- `double`: binary64; argumentos en `d0`, `d1`; retorno en `d0`. `f8–f15` (pares `d4–d7`) los conserva el llamado.
- `-fshort-double` hace `double` = `float` (sin cambiar la ABI de `float`).

## 7. Vídeo (NORMATIVA en niveles y registros de núcleo; PROVISIONAL en detalles de V2–V6)

### 7.1 Niveles

| Nivel | Nombre | Añade | Ejecutor HW |
| --- | --- | --- | --- |
| V0 | Terminal | Plano de texto (celdas 8×16 de 16 bits, por defecto, o de 32; fuente y paleta en VRAM, cursor) | compositor |
| V1 | Framebuffer | Plano bitmap (I1, I2, I4, I8, RGB565, ARGB1555, XRGB8888, ARGB8888; pitch; scroll; 1–3 búferes) y motor de copia | compositor |
| V2 | Retro 2D | 4 capas de tiles (8×8 o 16×16, 4 u 8 bpp) con scroll por capa y por línea; 1 capa afín; OAM de 128 sprites (hasta 64×64); 16 paletas de 16 + 1 de 256; tabla de líneas | compositor |
| V3 | Arcade 2D | Procesador de comandos; motor 2D (fill, blit con alfa/escala/rotación, líneas, triángulos, render a textura, clip, mezclas); 8 capas color verdadero; 1024 sprites afines; ventanas; mosaico; bilineal | sí |
| V4 | Vectorial 2D | Trazados (líneas, Bézier 2 y 3, arcos), relleno nonzero/evenodd, trazo, degradados, patrones, transformaciones 3×2, recortes, grupos Porter-Duff, texto por contornos | sí |
| V5 | 3D | Pipeline de función fija: vértices e índices, matrices, recorte, culling, Z D16/D32F, perspectiva, texturas con mipmaps, Gouraud, niebla, alpha test, mezcla | sí |
| V6 | 3D programable (opcional) | Shaders CSIR (vértice, píxel, compute) | sí |

`Mode` (0–6) elige el nivel; no puede superar `MaxLevel` (del perfil). Cada nivel incluye los anteriores.

- **Celda de texto de 16 bits** (la de por defecto, D20): bits 7:0 el carácter (glifo 0–255), 11:8 la tinta y
  15:12 el fondo (índices de las 16 primeras entradas de la paleta). La disposición de la celda de 32 bits es
  PROVISIONAL y se fija en F5.3.
- **Límites retro** (D17): el límite de sprites por línea del perfil (tabla de §4) se aplica en todos los perfiles.
  En `micro` y `pocket`, además, la VRAM sólo se puede escribir durante el VBlank; qué hace una escritura fuera de
  él se fija en F8.3.

### 7.2 Composición (orden fijo, de fondo a frente)

Color de fondo → plano bitmap (destino de V3–V6) → capas de tiles y sprites por prioridad (V2–V3) → plano de texto.
El scanout compone por línea, no consume ciclos de GPU y aplica los límites por línea del perfil.

### 7.3 Registros del núcleo de la GPU (`0xFF400000`)

| Offset | Nombre | Acceso | Descripción |
| --- | --- | --- | --- |
| `0x000` | Id | R | `0x55504743` («CGPU») |
| `0x004` | Version | R | mayor << 16 \| menor |
| `0x008` | Caps | R | b0–b6 niveles disponibles; b8–b15 formatos; b31 ejecutor hardware activo |
| `0x00C` | VramSize | R | bytes |
| `0x010` | GpuClockHz | R | |
| `0x014` | Mode | RW | 0–6 |
| `0x018` | MaxLevel | R | del perfil |
| `0x01C` | Control | RW | b0 pantalla activa; b1 procesador de comandos activo; b2 reset de la GPU |
| `0x020` | Status | R | b0 ocupada; b1 fallo; b2 en VBlank; b3 flip pendiente |
| `0x024` | IrqEnable | RW | b0 VBlank (32); b1 línea (33); b2 fence/copia (34); b3 fallo (35) |
| `0x028` | IrqStatus | W1C | |
| `0x02C` | FaultCode | R | |
| `0x030` | FaultAddress | R | |
| `0x100` | Width | RW | píxeles; ≤ resolución máxima del perfil |
| `0x104` | Height | RW | |
| `0x108` | Refresh | R | 50 o 60 |
| `0x10C` | LinesTotal | R | |
| `0x110` | VCount | R | línea actual |
| `0x114` | LineCompare | RW | IRQ 33 al llegar |
| `0x118` | FrameCounter | R | |
| `0x11C` | BackgroundColor | RW | XRGB8888 |
| `0x120` | Present | W | 1: aplicar las bases pendientes en el próximo VBlank |
| `0x200–0x23F` | Plano de texto | RW | Enable, Cols, Rows, CellsBase, CellFormat (0: 16 bits, 1: 32 bits), FontBase, GlyphCount, PaletteBase, CursorX, CursorY, CursorShape, ScrollbackBase, ScrollbackLines, ScrollY |
| `0x240–0x27F` | Plano bitmap | RW | Enable, Base, BackBase, Pitch, Format, Width, Height, ScrollX, ScrollY, Buffers, PaletteBase |
| `0x280–0x2BF` | Motor de copia | RW | Src, Dst, Length, FillValue, Command (1 copy, 2 fill), Status |
| `0x300–0x3FF` | V2–V3 | RW | Capas, capa afín, sprites, tabla de líneas (se fija en F8 y F10) |

Slots `0x41` (procesador de comandos: RingBase, RingSize, RingHead, RingTail, FenceCompleted, CmdStatus), `0x42`
(estado 2D y vectorial) y `0x43` (estado 3D): PROVISIONALES, se fijan en F10, F13 y F14.

### 7.4 VRAM al arrancar

La GPU arranca en V0 con la VRAM empaquetada desde `0xA0000000`, en este orden y alineada a 256 bytes: celdas
del terminal (máximo del perfil, 16 bits), fuente 8×16 de 256 glifos (4 KiB, 1 bpp, la de `text_font.h`),
paleta de 256 entradas (1 KiB; las 16 primeras, ANSI), scrollback (hasta 1/4 de la VRAM, máximo 256 KiB). Las
direcciones se publican en los registros del plano de texto; los programas las leen de ahí, no las suponen.

## 8. Terminal virtual (NORMATIVA)

### 8.1 Registros (`0xFF000000`)

| Offset | Nombre | Acceso | Descripción |
| --- | --- | --- | --- |
| `0x00` | Status | R | b0 hay entrada; b1 listo para salida; b2 fin de entrada (Ctrl+D); b3 Ctrl+C pendiente |
| `0x04` | Output | W | Byte bajo a la salida (UTF-8 + ANSI) |
| `0x08` | Input | R | Siguiente byte de entrada (0 si no hay) |
| `0x0C` | Available | R | Bytes de entrada disponibles |
| `0x10` | Mode | RW | b0 raw; b1 eco; b2 historial; b3 IRQ 19 al haber entrada |
| `0x14` | ErrorOutput | W | Byte bajo a la salida en el color de error |
| `0x18` | Control | RW | b0 activo; b1 cursor visible; b2 scrollback; b3 autoscroll |
| `0x1C` | Cols | R | |
| `0x20` | Rows | R | |
| `0x24` | CursorX | RW | |
| `0x28` | CursorY | RW | |
| `0x2C` | InterruptAck | W | Escribir 1 borra Ctrl+C pendiente |
| `0xF0` | BlockAddress | W | |
| `0xF4` | BlockLength | W | |
| `0xF8` | BlockCommand | W | 1 escribir bloque a la salida; 2 leer hasta `BlockLength` bytes de entrada; 3 escribir bloque de error |
| `0xFC` | BlockCount | R | Bytes movidos por el último bloque |

### 8.2 Salida

- UTF-8; un código sin glifo en la fuente se dibuja como `?`.
- Controles: `\r`, `\n` (baja y vuelve a la columna 0), `\b`, `\t` (a múltiplos de 8), `\a` (una nota A0 corta si hay audio).
- ANSI (ESC `[` …): `A`, `B`, `C`, `D` (mover), `H` y `f` (posición), `J` (0, 1, 2), `K` (0, 1, 2), `m` (SGR: 0, 1, 7,
  22, 27, 30–37, 39, 40–47, 49, 90–97, 100–107, `38;5;n`, `48;5;n`), `s`/`u` (guardar y restaurar), `?25l`/`?25h`
  (cursor), `r` (región de scroll). También `ESC 7` y `ESC 8`. Cualquier otra secuencia se consume sin efecto.
- `stderr` (`ErrorOutput` y el comando de bloque 3) se dibuja en el terminal con el color de error. No se copia
  al terminal del host (D21).
- Scrollback en VRAM; Shift+RePág y Shift+AvPág lo recorren en la ventana (no es visible para el programa).

### 8.3 Entrada

- Del texto del teclado de la ventana. Modo cocinado: eco, retroceso, Ctrl+U, flechas izquierda y derecha,
  historial con arriba y abajo, Enter entrega la línea. Ctrl+D al inicio de línea: fin de entrada. Ctrl+C:
  pone `Status.b3` y lanza la IRQ 19; la STDLIB lo convierte en `SIGINT`. Modo raw: cada tecla al momento.
- Bytes de cada tecla en los dos modos (D25; los mismos que da la VM v1 y los que decodifica `key.h`): el texto,
  en UTF-8; Esc `ESC`; flechas `ESC[A` (arriba), `ESC[B` (abajo), `ESC[C` (derecha), `ESC[D` (izquierda);
  Inicio `ESC[H`; Fin `ESC[F`; Insertar `ESC[2~`; Suprimir `ESC[3~`; RePág `ESC[5~`; AvPág `ESC[6~`. En modo
  cocinado, las teclas de edición las consume la disciplina de línea y no llegan al programa.

## 9. Audio (NORMATIVA en niveles; PROVISIONAL en registros hasta F9/F11)

| Nivel | Nombre | Contenido |
| --- | --- | --- |
| A0 | Tono | 1 voz; nota MIDI 0–127, velocidad, duración (ms virtuales), onda (cuadrada, triángulo, sierra, seno, ruido); cola de 64 notas; IRQ 28 al vaciarse |
| A1 | PSG | 4 voces: 2 cuadradas con ciclo de trabajo y barrido; 1 tabla de onda de 32 muestras de 4 bits; 1 ruido LFSR de 7 o 15 bits; envolventes; pan L/R |
| A2 | FM + MIDI | 8 voces FM de 4 operadores, 8 algoritmos, realimentación, ADSR por operador, LFO; secuenciador MIDI (16 canales, eventos en RAM con tiempos en ticks, banco FM General MIDI incorporado); IRQ 29 |
| A3 | Sampler | 32 voces PCM de 8 o 16 bits o IMA-ADPCM leídas de RAM; tono 16.16; bucle; ADSR; volumen y pan; paso bajo por voz; eco, reverb y chorus; bancos `.cbank` usables por el secuenciador |
| A4 | Audio digital | 8 flujos PCM (8/16 bits, mono/estéreo, 8–48 kHz) desde búferes circulares en RAM; IRQ 30 al necesitar datos; remuestreo a 48 kHz; decodificador IMA-ADPCM y QOA; EQ de 3 bandas, compresor/limitador, reverb |

- Mezclador: 48 kHz estéreo en tiempo virtual (evento del planificador por bloque de muestras). No consume
  ciclos de CPU; leer muestras de la RAM tampoco.
- Salida: `SdlAudio` con búfer elástico (remuestreo ligero para absorber la deriva con la tarjeta real);
  sin ventana, `--audio-out <archivo.wav>`.
- Slots: `0x20` control (Id, Caps, Level, MaxLevel, MasterVolume, BusVolume[5], IrqEnable, IrqStatus, SampleRate,
  SampleCounter), `0x21` voces, `0x22` secuenciador, `0x23` flujos y efectos.

## 10. Línea de órdenes (NORMATIVA)

| Opción | Efecto |
| --- | --- |
| `--profile <nombre>` | Sección 4 |
| `--cpu-clock`, `--gpu-clock`, `--ram`, `--vram`, `--max-video`, `--max-audio`, `--max-resolution` | Sueltas; convierten el perfil en `custom` |
| `--speed realtime\|max\|<f>x` | Sección 3.3 |
| `--refresh 50\|60`, `--rtc <fecha>` | Sección 3.1 |
| `--headless` | Sin ventana (también `CERES_HEADLESS=1`) |
| `--gpu auto\|software\|hardware` | Ejecutor de la GPU; sin ventana siempre software |
| `--transcript <archivo>` | Bytes escritos en el terminal virtual; los de error, entre `\x1b[E` y `\x1b[e` |
| `--screen-log <archivo>` | El plano de texto como texto plano en cada `Present` |
| `--frames <dir>` | Un PNG por `Present` (V1+) |
| `--audio-out <archivo.wav>` | La mezcla de audio |
| `--type <archivo>` | Texto tecleado al arrancar (sustituye a `.stdin`) |
| `--keys <archivo>` | Eventos de teclado con su instante virtual |
| `--record <archivo>`, `--replay <archivo>` | Entrada sellada |
| `--log <archivo>` | Registro de depuración y diagnósticos a archivo |
| `--strict-mmio` | Sección 5.1 |
| `--fullscreen` | Ventana a pantalla completa (también F11) |
| `--exit-on-halt` | Cerrar la ventana al terminar el programa |
| `--shell` | Volver al shell al terminar el programa |

Se eliminan `--terminal` y la entrada de consola del host. `ceres run` sin programa arranca el shell.

## 11. STDLIB por niveles (NORMATIVA en nombres; contenido en cada fase)

| Cabecera | Nivel | Sustituye a |
| --- | --- | --- |
| `ceres.h` (bases MMIO, `mmio_r32`/`mmio_w32`) | — | quita `mmio_r8`, `mmio_r16`, `mmio_w8`, `mmio_w16` |
| `ceres/terminal.h` | V0 | la versión actual (ahora sobre el terminal virtual) |
| `ceres/debug.h` | — | ahora al registro de depuración; `dbg_break()` |
| `ceres/timer.h` | — | ciclos y tiempo virtual; sin `timer_halt_clock` |
| `ceres/video.h` | todos | nuevo: nivel, resolución, VBlank, `video_present`, `video_wait_vblank` |
| `ceres/text.h` | V0 | `ceres/textfb.h` |
| `ceres/fb.h` | V1 | `ceres/display.h` |
| `ceres/tiles.h`, `ceres/sprite.h` | V2–V3 | `sprite.h` actual (los sprites por software pasan a `gfx.h`) |
| `ceres/gpu.h`, `ceres/gpu2d.h` | V3 | `ceres/blitter.h` |
| `ceres/canvas.h` | V4 | nuevo |
| `ceres/gpu3d.h`, `vecmath.h` con `mat4` | V5 | nuevo |
| `ceres/tone.h`, `music.h` | A0 | parte de `audio.h` |
| `ceres/psg.h` | A1 | parte de `audio.h` |
| `ceres/fm.h`, `ceres/midi.h` | A2 | nuevo |
| `ceres/sampler.h` | A3 | nuevo |
| `ceres/pcm.h`, `ceres/sound.h`, `ceres/wav.h`, `ceres/qoa.h` | A4 | nuevo |
| — | — | se retiran `f64.h` y `ns64.h` (F6) |

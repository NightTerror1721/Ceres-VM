# Tutorial práctico: de "Hola, Ceres" a dos juegos de terminal

[← Volver al índice](README.md)

> Este tutorial está en español porque nació de una petición concreta: aprender CeresASM
> practicando con ejercicios progresivos que terminan en dos juegos jugables por terminal
> (piedra-papel-tijera y tres en raya). El resto de la wiki (páginas 01-19) es la referencia en
> inglés; esta página es el camino guiado que la recorre en la práctica.

Todo el código de este tutorial vive en
[`Ceres-ASM/examples/tutorial/`](../Ceres-ASM/examples/tutorial/), listo para ensamblar y
ejecutar tal cual. Cada ejercicio es un `.casm` independiente con comentarios y una sección
"PRUEBA TÚ MISMO" al principio para extenderlo.

## 0. Antes de nada: compilar `ceres` y una entrada de teclado que sí funciona

Sigue el [`README`](../README.md) para compilar:

```sh
cd Ceres-ASM/src
g++ -std=c++23 -I. -o ceres main.cpp vm/*.cpp assembler/*.cpp
```

**Nota importante si tu copia de `ceres` es de antes de este tutorial:** `ceres run` no leía el
teclado real. El dispositivo de terminal (`TerminalDevice`, puerto `0x02`) siempre tuvo un buffer
de entrada pensado para recibir datos de forma concurrente (`pushInput`, protegido con atómicos),
pero nada en `main.cpp` lo conectaba con `stdin`. Sin eso, ningún programa interactivo —y por
tanto ninguno de los dos juegos— podía leer lo que el jugador escribe. Se ha añadido un hilo en
segundo plano en `runProgram()` que lee de `stdin` y llama a `terminal.pushInput()`, reutilizando
el buffer que ya existía. Si tu binario es más reciente que este cambio, ya tienes entrada de
teclado funcional con `ceres run juego.casm`; si no, recompílalo.

Comprueba que funciona:

```bash
ceres run examples/tutorial/02_eco.casm
```

Escribe una frase y pulsa Intro: el programa te la devuelve carácter a carácter.

## 1. Cómo funciona la entrada por teclado en Ceres (léelo antes del ejercicio 2)

La terminal de Ceres **se almacena por línea**, igual que cualquier shell: un programa no recibe
nada por el puerto `0x02` (`TERM_IN`) hasta que el usuario pulsa Intro, y entonces recibe toda la
línea de golpe, incluido el propio `\n` (código 10). El puerto `0x00` (`TERM_STATUS`) tiene el bit
0 a uno cuando hay al menos un byte pendiente; leer `TERM_IN` sin comprobarlo antes simplemente
devuelve `0` si no hay nada.

Esto tiene una consecuencia práctica: si tu programa solo lee **un** carácter por turno (por
ejemplo, la jugada de piedra-papel-tijera), el `\n` que el usuario escribió después se queda en el
buffer y se cuela como si fuera el siguiente dato leído. La solución que usan todos los ejercicios
a partir del 3 es una subrutina `read_command` que lee el primer carácter con el que se queda, y
luego **descarta el resto de la línea hasta encontrar el `\n`**:

```casm
read_command:
.wait_first:
    inb TERM_STATUS, r1
    and r1, r1, 1
    jz .wait_first
    inb TERM_IN, r0        // r0 = primer byte (valor de retorno)
.flush:
    inb TERM_STATUS, r1
    and r1, r1, 1
    jz .flush
    inb TERM_IN, r2
    cmp r2, 10
    jnz .flush
    ret
```

## 2. Dos correcciones a la referencia de instrucciones

Mientras se escribían los ejercicios aparecieron dos sitios donde el código real (verificado
ensamblando y ejecutando) no coincide con lo que dice
[05 · Instruction set](05-Instruction-Set.md) — ya corregido en esa página, pero merece
explicarse aquí porque es fácil tropezar con lo mismo:

- **`in`/`inb`/`inh`/`insb`/`insh`**: el puerto va **primero**, el registro destino **segundo** —
  `inb PUERTO, rd`, no `inb rd, PUERTO` como decía la tabla. Es coherente con `out`
  (`outb PUERTO, rs`): en los dos casos el puerto se escribe primero.
- **`str`/`strb`/`strh`**: el valor va **primero**, la dirección entre corchetes **segunda** —
  `strb rs, [rd + imm16]`, no `strb [rd + imm16], rs`. De hecho, el ejemplo
  [`examples/test.casm`](../Ceres-ASM/examples/test.casm) tenía este mismo error y ni siquiera
  llegaba a ensamblar; también se ha corregido.

Los ejercicios de este tutorial ya usan el orden correcto en los dos casos, comprobado ensamblando
y ejecutando cada uno.

## 3. Los ejercicios

| # | Archivo | Qué practica |
| --- | --- | --- |
| 1 | [`01_hola.casm`](../Ceres-ASM/examples/tutorial/01_hola.casm) | `@rodata`, `la`, imprimir un string por el puerto de terminal |
| 2 | [`02_eco.casm`](../Ceres-ASM/examples/tutorial/02_eco.casm) | Leer de `TERM_IN`/`TERM_STATUS`, byte a byte |
| 3 | [`03_suma.casm`](../Ceres-ASM/examples/tutorial/03_suma.casm) | `read_command`, conversión ASCII↔número, `cmp`+`jc` |
| 4 | [`04_cuenta_atras.casm`](../Ceres-ASM/examples/tutorial/04_cuenta_atras.casm) | Bucles con `cmp`/`jz`/`jnz` |
| 5 | [`05_notas.casm`](../Ceres-ASM/examples/tutorial/05_notas.casm) | Arrays en `@data`, direccionamiento `[reg + offset]`, `div`/`mod` |
| 6 | [`06_macros.casm`](../Ceres-ASM/examples/tutorial/06_macros.casm) | `macro`, convención de llamada `proc_enter`/`proc_leave` |
| 7 | [`07_aleatorio.casm`](../Ceres-ASM/examples/tutorial/07_aleatorio.casm) | Un generador congruencial lineal sembrado con `RTC_TIME` |

Para cada uno:

```bash
ceres run examples/tutorial/0N_nombre.casm
```

y lee el comentario de cabecera antes de mirar el código: plantea un reto concreto para que lo
resuelvas antes de ver cómo lo resolvió este tutorial.

### Nota sobre el ejercicio 7: por qué la semilla es `RTC_TIME` (puerto `0x11`) y no `SYS_TICKS` (puerto `0x10`)

Ceres no tiene una fuente de números aleatorios real — el puerto `0xFE` está reservado pero sin
implementar (ver [07 · I/O devices and ports](07-IO-Devices-and-Ports.md)). La tentación es sembrar
un generador propio con el contador de instrucciones ejecutadas (`SYS_TICKS`), pero
[02 · Memory](02-Memory.md) y [07 · I/O devices and ports](07-IO-Devices-and-Ports.md) ya avisan de
que el tiempo en Ceres se cuenta en instrucciones ejecutadas, no en tiempo real, precisamente para
que un programa se comporte igual en cada ejecución — y eso incluye `SYS_TICKS`. Sembrar con él da
literalmente la misma "aleatoriedad" cada vez que ejecutas el programa (se puede comprobar
ejecutando `07_aleatorio.casm` dos veces seguidas). El único valor de toda la máquina que de verdad
cambia es el reloj de pared en segundos, `RTC_TIME` (puerto `0x11`) — la propia wiki lo señala como
"lo único que aquí no es determinista". Por eso es la semilla que usan el ejercicio 7 y, más
adelante, piedra-papel-tijera.

## 4. Los dos juegos

### Piedra, papel o tijera — [`piedra_papel_tijera.casm`](../Ceres-ASM/examples/tutorial/piedra_papel_tijera.casm)

Contra la máquina. Junta la lectura de teclado (2-3), la aritmética con acarreo (3-4), un
marcador en memoria (5), macros (6) y el generador aleatorio (7). La regla de quién gana se reduce
a una única cuenta modular: con `0=Piedra, 1=Papel, 2=Tijera`, la jugada `i` vence a la jugada
`(i - 1 + 3) mod 3`, así que

```casm
sub r6, jugador, maquina
add r6, r6, 3
mod r6, r6, 3      // 0 = empate, 1 = ganas, 2 = pierdes
```

decide la ronda sin una cascada de comparaciones para las 9 combinaciones posibles.

```bash
ceres run examples/tutorial/piedra_papel_tijera.casm
```

### Tres en raya — [`tres_en_raya.casm`](../Ceres-ASM/examples/tutorial/tres_en_raya.casm)

Dos jugadores humanos turnándose en la misma terminal (no hay IA — el reto 3 de su cabecera
propone añadirla reutilizando `next_random`). El tablero es un array de 9 bytes en `@data`
(`0`=vacía, `1`=X, `2`=O) direccionado con desplazamientos calculados en tiempo de ejecución
(a diferencia del array de notas del ejercicio 5, aquí el índice no se conoce hasta que el
jugador lo escribe). Comprobar quién gana recorre una tabla de 8 combinaciones ganadoras
(`WIN_LINES`, en `@rodata`) con un bucle, en vez de escribir a mano las 8 comparaciones.

```bash
ceres run examples/tutorial/tres_en_raya.casm
```

**Una trampa en la que es fácil caer** al escribir subrutinas como `check_winner` que usan varios
registros como variables temporales: si el código que las llama todavía necesita el valor que
tenía un registro *antes* de la llamada (por ejemplo, de quién era el turno, o el resultado que
acaba de devolver la subrutina anterior), tienes que guardarlo en un registro que la subrutina no
toque — o releerlo de memoria — **antes** de hacer cualquier otra llamada. `tres_en_raya.casm`
tropezó exactamente con esto dos veces mientras se escribía (el turno no cambiaba nunca, y el
ganador se anunciaba al revés) porque `print_board` y `check_winner` reutilizan registros que el
bucle principal todavía necesitaba; los comentarios `mov r11, r0` y `ldv r4, turn` que verás en el
código son la solución. Es el mismo problema que resuelve la convención `proc_enter`/`proc_leave`
del ejercicio 6, aplicado a por qué hace falta una convención en primer lugar.

## 5. De aquí en adelante

- [14 · Macros](14-Macros.md) — la convención de llamada completa (`r0`-`r3`, `r4`-`r9`) de la que
  toma la idea el ejercicio 6.
- [15 · Modules and import](15-Modules-and-Import.md) — un reto natural: extraer `read_command`,
  `print`, `strlen` y `next_random` a un archivo `runtime.casm` común e importarlo desde los dos
  juegos con `import`, en vez de tenerlos duplicados en cada uno.
- [19 · Known limitations](19-Known-Limitations.md) — por qué no hay ninguna convención de llamada
  impuesta por la máquina, y qué otros dispositivos (aparte del terminal y el timer) siguen sin
  implementarse.

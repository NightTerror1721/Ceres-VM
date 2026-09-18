# Hoja de ruta educativa y retro para Ceres

[← Back to index](README.md)

## 1. Propósito del documento

Este documento define una dirección de evolución para Ceres orientada deliberadamente a tres objetivos:

1. Servir como plataforma educativa para aprender arquitectura de computadores, ensambladores,
   linkers, memoria, interrupciones, dispositivos y conceptos básicos de sistemas operativos.
2. Ser un entorno lúdico donde resulte sencillo escribir programas interactivos, demos y pequeños
   juegos de terminal.
3. Permitir construir juegos con gráficos sencillos, inspirados en consolas retro, sin convertir el
   proyecto en una plataforma profesional ni perseguir compatibilidad con arquitecturas comerciales.

La meta no es ejecutar aplicaciones grandes, competir con una arquitectura real ni proporcionar un
entorno de producción. La meta es que una persona pueda entender la máquina, modificarla, observar
lo que ocurre y crear algo que funcione de principio a fin.

La medida principal de éxito debería ser:

> Una persona puede leer la documentación, escribir un programa pequeño, entender sus errores y
> construir un juego o un kernel sencillo sin tener que conocer miles de detalles ocultos.

## 2. Principios de diseño

### 2.1. Comprensible antes que completo

Ceres debe preferir una arquitectura explicable a una arquitectura con más características pero más
difícil de razonar. Cada nuevo subsistema debería responder claramente a estas preguntas:

- ¿Qué concepto educativo enseña?
- ¿Qué programas permite construir?
- ¿Cómo se observa durante la ejecución?
- ¿Cuál es su comportamiento cuando algo sale mal?
- ¿Puede explicarse en una sección corta de la documentación?

No es necesario añadir todas las características que tendría un ordenador real. Es mejor tener pocos
dispositivos con una interfaz consistente que muchos dispositivos parcialmente documentados.

### 2.2. Los errores también forman parte de la experiencia

Un fallo del programa no debería producir únicamente un carácter genérico o una parada silenciosa.
Los errores deben ayudar a descubrir conceptos como alineación, tamaño de acceso, dirección de retorno,
vector de interrupción o límite de memoria.

El sistema debe ser estricto para que los programas aprendan las reglas reales, pero el diagnóstico
debe ser suficientemente claro para que el usuario pueda corregirlos.

### 2.3. Evolución por capas

La evolución recomendada tiene cuatro capas:

1. **Fundación:** diagnósticos, documentación, convenciones y estabilidad del flujo existente.
2. **Biblioteca:** rutinas reutilizables para entrada, salida, memoria, temporización y gráficos.
3. **Plataforma de juegos:** framebuffer, colores, doble buffer, teclado, sonido y ejemplos.
4. **Plataforma de sistemas:** kernels pequeños, tareas, memoria virtual, drivers y programas de usuario.

Las capas superiores no deberían ocultar las inferiores. Un juego puede usar una biblioteca cómoda,
pero el usuario debe poder descender hasta el MMIO y entender qué está ocurriendo.

### 2.4. Reproducibilidad y experimentación

La ejecución debe poder repetirse de forma determinista cuando el programa no depende de entrada
externa. El temporizador, el perfilador, el debugger y los tests deben usar esa propiedad para que un
estudiante pueda repetir un experimento y obtener el mismo resultado.

## 3. Mejoras prioritarias de la experiencia de ejecución

### 3.1. Diagnósticos de excepciones mucho más informativos

El stub de la BIOS no debería limitarse a escribir `E` cuando se produce una excepción. Ese carácter
puede mantenerse como representación mínima, pero Ceres debería ofrecer un modo de diagnóstico
detallado que muestre al menos:

- nombre de la excepción;
- número de interrupción;
- PC en el que se produjo;
- instrucción que estaba ejecutándose;
- dirección de memoria implicada, si existe;
- tamaño y tipo del acceso, si existe;
- estado relevante de los flags;
- valor de los registros principales;
- profundidad de interrupción;
- archivo y línea de origen cuando existe información de debug.

Un ejemplo de salida útil sería:

```text
AlignmentFault
  pc:       0x00000574
  source:   io.casm:53
  access:   store word
  address:  0x00000801
  reason:   word access requires a 4-byte aligned address
  hint:     use `char [r2]` or `strb` for byte data
```

Esto habría hecho evidente el problema de `gets`: el programa escribía una palabra en una dirección
que avanzaba de byte en byte.

### 3.2. Modos de ejecución observables

El comando `run` debería ofrecer modos opcionales para aprender y depurar:

- `--trace`: imprime cada instrucción o cada instrucción relevante;
- `--trace-mmio`: muestra lecturas y escrituras a dispositivos;
- `--trace-interrupts`: muestra cuándo se solicita, acepta, enmascara y retorna una interrupción;
- `--max-instructions N`: detiene programas que entren en un bucle accidental;
- `--dump-on-fault`: vuelca registros, pila y PC al producirse una excepción;
- `--quiet` y `--verbose`: permiten controlar el nivel de información.

El trace no debería ser obligatorio durante el uso normal. Debe ser una herramienta explícita para
que un alumno pueda observar el camino entre una instrucción y el estado de la máquina.

### 3.3. Razones de parada diferenciadas

La VM debería distinguir claramente entre:

- programa terminado mediante `SystemControlDevice`;
- programa detenido temporalmente por `halt`;
- programa detenido por una excepción;
- programa detenido por un límite de instrucciones;
- programa detenido por una orden del debugger;
- programa detenido porque el host perdió la entrada o cerró el dispositivo.

Esto evita que un programa que está esperando deliberadamente una interrupción parezca haber fallado.

### 3.4. Inspección del estado final

Además del código de salida, el CLI podría permitir inspeccionar:

- registros generales y flotantes;
- flags;
- PC y SP;
- mapa de memoria cargado;
- vectores instalados;
- estadísticas de instrucciones;
- bytes pendientes en el terminal;
- bytes descartados por buffers llenos.

Esta información sería útil tanto para debugging como para ejercicios de arquitectura.

## 4. Mejoras del lenguaje ensamblador CASM

### 4.1. Hacer explícito el tamaño de los accesos

El lenguaje ya distingue accesos de byte, halfword y word, pero la sintaxis debe ser especialmente
clara para alguien que está aprendiendo. Se recomienda documentar y favorecer una convención uniforme:

```casm
mov char[r2], r0       // un byte
mov half[r2], r0       // dos bytes
mov [r2], r0           // una palabra
```

Los errores de tamaño y alineación son parte del aprendizaje, pero la sintaxis no debería ocultar el
tamaño que el ensamblador ha elegido.

### 4.2. Diagnósticos de tamaño y alineación

El ensamblador podría emitir warnings en casos sospechosos:

- almacenar una palabra en una dirección que se incrementa mediante `inc` dentro de un bucle;
- copiar un valor declarado como `byte` con un acceso de palabra;
- reservar un buffer de caracteres sin espacio para el terminador;
- usar un acceso de palabra sobre un símbolo con tipo byte;
- leer un registro MMIO con un tamaño no soportado por el dispositivo.

No todos estos casos pueden determinarse con certeza estática, por lo que algunos deben ser warnings
opcionales y no errores.

### 4.3. Anotaciones y ayudas de tipo

Sería útil permitir anotaciones sencillas para comunicar intención:

```casm
let input: byte[128]
let cursor: ptr
```

El ensamblador podría usar esa información para detectar ciertos accesos incompatibles y proporcionar
mejores sugerencias sin convertir CASM en un lenguaje de alto nivel.

### 4.4. Convenciones ABI más visibles

La convención de llamadas debe estar presente en los ejemplos y en los mensajes del debugger:

- registros de argumentos;
- registros de retorno;
- registros que una función debe preservar;
- uso de `sp` y `fp`;
- comportamiento de funciones variádicas;
- reglas para funciones que pueden ser interrumpidas.

Cada rutina de la biblioteca estándar debería indicar explícitamente qué registros modifica.

### 4.5. Bibliotecas de bajo nivel reutilizables

El proyecto debería mantener una biblioteca CASM pequeña, coherente y bien documentada con módulos
para:

- memoria: `memcpy`, `memset`, comparación y movimiento de bytes;
- cadenas: longitud, copia, comparación y concatenación;
- terminal: caracteres, líneas, números y formatos sencillos;
- temporizador: esperas y ticks;
- framebuffer: dibujo de celdas, texto y rectángulos;
- matemáticas: enteros, aleatoriedad reproducible y utilidades geométricas;
- estructuras: colas, pilas, buffers circulares y mapas sencillos.

La biblioteca no debe ocultar el hardware completamente. Cada función debería indicar qué registros
MMIO utiliza y ofrecer un ejemplo equivalente escrito directamente con cargas y stores.

## 5. Entrada, interrupciones y terminal

### 5.1. Definir claramente el modelo de entrada

La documentación debe explicar de forma inequívoca:

- que `stdin` del host se lee en segundo plano;
- que la entrada puede ser line-buffered;
- que pulsar Enter puede introducir varios bytes a la vez;
- que `pushInput()` puede generar una sola interrupción para varios bytes;
- que la interrupción indica disponibilidad, no necesariamente un único carácter;
- que el ring buffer conserva los bytes pendientes.

Esta distinción es esencial: una ISR no debe asumir que cada interrupción representa exactamente un
carácter.

### 5.2. Rutina estándar de lectura bloqueante

La biblioteca debería ofrecer una rutina de referencia con este comportamiento:

1. Comprobar si la ISR dejó un carácter capturado.
2. Si no lo hizo, comprobar `StatusRegister`.
3. Si no hay entrada, ejecutar `halt`.
4. Al despertar, volver a comprobar el estado.
5. Leer el carácter capturado o el siguiente carácter de `InputRegister`.
6. Repetir hasta obtener un dato válido.

La rutina debe ser segura cuando llegan varios bytes juntos y no debe perder el resto de la línea.

### 5.3. Buffer de eventos de entrada

Para programas más avanzados, una sola variable como `term_isr_char` es limitada. Sería recomendable
proporcionar una biblioteca que mantenga un buffer de eventos o caracteres en RAM:

- la ISR recoge o señala la entrada;
- el código principal drena el dispositivo;
- el buffer conserva los caracteres pendientes;
- las funciones de alto nivel pueden leer sin depender de una única variable global.

La ISR debería ser corta y predecible. Cuanto menos trabajo haga dentro de una interrupción, más fácil
será explicar su comportamiento y evitar problemas de reentrada.

### 5.4. Pruebas específicas de entrada

Debe haber pruebas para:

- un carácter único;
- varios caracteres en una sola llamada a `pushInput`;
- una línea terminada en `LF`;
- una línea vacía;
- EOF;
- buffer lleno;
- varias interrupciones antes de que el programa ejecute `getchar`;
- entrada mientras la máquina está en `halt`;
- entrada con interrupciones temporalmente enmascaradas.

## 6. Terminal, framebuffer y API gráfica retro

### 6.1. Separar consola y pantalla de juego

Ceres debería mantener dos conceptos distintos:

- **Terminal:** stream de caracteres para comandos, logs y texto que avanza.
- **Framebuffer:** superficie redibujable para el estado visual del juego.

La terminal puede servir como consola de depuración mientras el framebuffer muestra el juego.

### 6.2. Mejoras mínimas del framebuffer

Para juegos retro de caracteres, el framebuffer debería ofrecer:

- tamaño configurable de columnas y filas;
- escritura de una celda individual;
- escritura de texto;
- limpieza completa;
- presentación explícita;
- coordenadas con límites comprobados;
- colores de texto y fondo;
- atributos por celda;
- doble buffer opcional.

La operación de presentar debería ser independiente de modificar el buffer. Así se evita que el

### 6.3. Doble buffer

El doble buffer permitiría:

1. Dibujar el frame completo en un buffer oculto.
2. Intercambiar buffers.
3. Presentar una imagen consistente.

Es un concepto importante para explicar rendering y elimina parpadeos en juegos con animación.

### 6.4. Primitivas de dibujo

La biblioteca gráfica debería incluir rutinas sencillas para:

- dibujar una celda;
- dibujar una línea horizontal o vertical;
- dibujar rectángulos;
- copiar regiones;
- dibujar sprites de caracteres;
- escribir texto con clipping;
- borrar una región;
- mostrar números y puntuaciones.

Estas primitivas pueden estar implementadas en CASM al principio. No hace falta crear una GPU real.

### 6.5. Entrada específica para juegos

La entrada orientada a juegos debería distinguir al menos:

- tecla pulsada;
- tecla liberada, si el host lo permite;
- repetición;
- Enter, Escape y espacio;
- flechas o equivalentes;
- códigos especiales.

Si el terminal sigue siendo line-buffered, debe existir una modalidad de entrada inmediata para juegos.
No es conveniente construir un juego en tiempo real dependiendo de que el usuario pulse Enter.

### 6.6. Sonido sencillo

Como ampliación opcional, un `SoundDevice` mínimo podría ofrecer:

- frecuencia;
- duración;
- volumen;
- forma de onda básica;
- cola de efectos;
- canal de música sencillo.

Incluso un dispositivo limitado a tonos sería suficiente para enseñar periféricos y mejorar mucho la


### 7.1. Reloj y ticks

Los juegos necesitan una noción clara de tiempo. El temporizador debería documentar:

- unidad de tiempo;
- frecuencia o número de instrucciones;
- modo periódico;
- vector utilizado;
- comportamiento cuando el programa está detenido;
- relación entre tick y frame.

La ejecución por número de instrucciones es excelente para reproducibilidad, pero debe diferenciarse

### 7.2. API de espera

La biblioteca podría ofrecer:

- `wait_ticks(n)`;
- `sleep_until_tick(t)`;
- `get_ticks()`;
- `wait_for_input_or_tick()`.

La última función sería especialmente útil para juegos por turnos e interfaces interactivas.


Los ejemplos deberían converger en un bucle como este:

```text
inicializar estado
inicializar framebuffer
inicializar timer

while running:
    procesar entrada pendiente
    actualizar lógica usando delta o ticks
    dibujar el estado completo
    presentar el frame
    esperar el siguiente tick
```

Esto enseña la diferencia entre entrada, actualización, rendering y sincronización.


### 8.1. Arranque y modos de ejecución

La documentación debería incluir un tutorial específico de kernel, separado del tutorial de juegos.
Debería explicar:

- dirección de entrada;
- inicialización de registros;
- stack inicial;
- vector de reset;
- handlers de excepciones;
- inicialización de dispositivos;
- bucle principal del kernel;
- apagado controlado.

### 8.2. Scheduler cooperativo

Un primer scheduler educativo no necesita preempción completa. Puede incluir:

- tareas representadas por contextos;
- cambio explícito de tarea;
- pila por tarea;
- estado listo y bloqueado;
- yield cooperativo;
- espera por timer o entrada.

Después podría añadirse preempción por timer como ejercicio avanzado.


Una evolución natural es definir un pequeño conjunto de llamadas al sistema:

- escribir texto;
- leer entrada;
- reservar memoria;
- crear o terminar tarea;
- esperar un tick;
- acceder al framebuffer;
- abrir y leer un archivo virtual.

El objetivo no sería seguridad industrial, sino demostrar la frontera entre kernel y programa de


Se podrían incluir ejercicios progresivos:

1. allocator lineal;
2. free list;
3. bloques de tamaño fijo;
4. heap del kernel;
5. memoria virtual para un proceso sencillo;
6. protección de páginas;
7. fallo de página controlado.

Cada ejercicio debería poder ejecutarse y observarse con el debugger.


Cada dispositivo debería tener un ejemplo de driver CASM o pseudokernel:

- terminal;
- timer;
- disco;
- framebuffer;
- DMA;
- sonido, si se añade.

El ejemplo debe enseñar registros, estados, interrupciones, errores y sincronización.


### 9.1. Ruta de ejemplos por dificultad

La colección de ejemplos debería crecer siguiendo esta secuencia:

1. imprimir un carácter;
2. imprimir una cadena;
3. leer una línea;
4. contador y bucles;
5. arrays y cadenas;
6. funciones y stack frames;
7. interrupción de terminal;
8. juego por turnos;
9. temporizador;
10. framebuffer;
11. animación;
12. juego completo;
13. kernel mínimo;
14. scheduler cooperativo.

### 9.2. Juegos de terminal

Los primeros juegos deberían ser pequeños y completamente legibles:

- Rock Paper Scissors;
- Tic-Tac-Toe;
- Hangman;
- aventura de texto;
- Mastermind;
- Blackjack sencillo;
- roguelike mínimo.

Cada juego debería destacar un concepto técnico concreto en lugar de añadir complejidad arbitraria.

### 9.3. Juegos gráficos retro

Una secuencia razonable sería:

- Snake para practicar coordenadas y colisiones;
- Pong para practicar tiempo y movimiento;
- Breakout para practicar arrays de entidades;
- Tetris para practicar matrices y rotaciones;
- Space Invaders para practicar sprites y proyectiles;
- plataformas simples para practicar física discreta.

Cada ejemplo debería incluir una versión mínima y una sección de ampliaciones.


### 10.1. Vistas prioritarias

El debugger debería mostrar de manera integrada:

- código fuente actual;
- instrucción decodificada;
- registros;
- flags;
- PC, SP y FP;
- pila;
- memoria alrededor de una dirección;
- mapa de secciones;
- dispositivos MMIO;
- interrupciones pendientes;
- tareas, si existe scheduler.

### 10.2. Breakpoints educativos

Además de breakpoints por línea, serían útiles:

- breakpoints por dirección;
- breakpoints por acceso MMIO;
- breakpoints por interrupción;
- breakpoints por escritura en una dirección;
- breakpoints por excepción;
- ejecución hasta retorno;
- ejecución hasta cambio de registro.

### 10.3. Ejecución reversible

El historial existente puede convertirse en una gran característica didáctica. Las operaciones
prioritarias serían:

- paso atrás;
- volver al último breakpoint;
- comparar registros entre dos puntos;
- comparar memoria;
- mostrar qué instrucción produjo un cambio.

Esto es especialmente útil para aprender stack frames, interrupciones y fallos de memoria.


### 11.1. Tests de integración obligatorios

Además de las pruebas unitarias actuales, conviene mantener escenarios completos para:

- `stdin` del host;
- `TerminalDevice`;
- interrupción de entrada;
- `getchar` bloqueante;
- lectura de líneas con varios bytes;
- buffer lleno;
- final de línea;
- salida mediante terminal;
- framebuffer y presentación;
- timer y espera;
- ejecución de juegos de ejemplo.

### 11.2. Tests de regresión de errores conocidos

El caso de `gets` debe quedar protegido por un test que use un buffer de bytes y una entrada de varios
caracteres. Debe comprobar que:

- no aparece `AlignmentFault`;
- la cadena queda terminada;
- el `LF` se trata según la documentación;
- el programa continúa después de leer;
- la salida coincide con la entrada esperada.

### 11.3. Determinismo

Los tests deberían distinguir entre:

- comportamiento determinista del VM;
- comportamiento dependiente del host;
- comportamiento de entrada interactiva.

La entrada debe poder inyectarse directamente en el dispositivo para que la mayoría de los tests no
dependan de pipes, consolas o detalles del sistema operativo.

### 11.4. Pruebas de documentación

Los ejemplos de la documentación deberían ensamblarse automáticamente en CI. Un ejemplo que deja de
compilar es un fallo real de la documentación, no solo un problema editorial.


### 12.1. Manual por conceptos

La documentación actual debería complementarse con explicaciones progresivas:

- qué ocurre al arrancar una máquina;
- cómo una instrucción llega al dispositivo;
- cómo se ejecuta una llamada;
- cómo se instala una interrupción;
- cómo se dibuja un frame;
- cómo se cambia de tarea.

Cada concepto debe enlazar la explicación conceptual con la referencia técnica correspondiente.

### 12.2. Tutoriales separados

Se recomiendan cuatro rutas:

1. **Ruta de ensamblador:** instrucciones, datos, funciones y módulos.
2. **Ruta de dispositivos:** MMIO, terminal, timer, disco y framebuffer.
3. **Ruta de juegos:** input, bucle principal, dibujo y colisiones.
4. **Ruta de kernel:** arranque, interrupciones, memoria, tareas y syscalls.

### 12.3. Plantillas de proyectos

Deberían existir plantillas mínimas para:

- programa de terminal;
- juego por turnos;
- juego con framebuffer;
- kernel mínimo;
- driver de dispositivo;
- biblioteca CASM reutilizable.

Cada plantilla debería compilarse con un comando corto y tener un README local.


### Fase 1: robustez y comprensión

Prioridad máxima:

- diagnósticos detallados de faults;
- modo trace de instrucciones, MMIO e interrupciones;
- razón de parada;
- documentación del modelo de entrada;
- biblioteca de terminal corregida y probada;
- tests de regresión para accesos byte/word;
- límite de instrucciones en `run`.

Resultado esperado: los errores dejan de parecer misteriosos y los programas pequeños son fáciles de
depurar.

### Fase 2: biblioteca y ejemplos

- biblioteca estable de memoria y cadenas;
- terminal de alto nivel;
- temporizador de alto nivel;
- ejemplos progresivos;
- plantillas de proyecto;
- tutoriales separados para juegos y kernels.

Resultado esperado: una persona puede crear programas útiles sin reimplementar siempre las mismas
rutinas, aunque sigue pudiendo estudiar su implementación.

### Fase 3: consola retro

- framebuffer con colores;
- doble buffer;
- texto y primitivas de dibujo;
- entrada inmediata;
- ticks y sincronización de frames;
- Snake, Pong y Tetris como ejemplos;
- documentación de la API gráfica.

Resultado esperado: Ceres funciona como una pequeña consola retro programable en CASM.

> **Progreso:** la base ya está hecha. El `DisplayDevice` de píxeles, el teclado/ratón/gamepad
> reales y la ventana SDL (`ceres run --window`) implementan la capa de entrada/salida que esta fase
> asume — ver [I/O devices and ports](07-IO-Devices-and-Ports.md) y el
> [plan de integración de SDL3](29-SDL3-Integration-Plan.md). Quedan por encima las bibliotecas de
> dibujo, el doble buffer, los juegos de ejemplo y la documentación de la API gráfica.

### Fase 4: kernels educativos

- tutorial de arranque;
- allocator lineal y heap;
- scheduler cooperativo;
- tareas y stacks;
- syscalls mínimas;
- drivers de terminal y framebuffer;
- filesystem virtual simple.

Resultado esperado: se pueden practicar conceptos de sistemas operativos sin perder la capacidad de
inspeccionar toda la máquina.

### Fase 5: ampliaciones opcionales

- sonido sencillo;
- ejecución reversible más profunda;
- entrada de teclado inmediata multiplataforma;
- sprites o tiles más estructurados;
- herramientas de perfiles gráficos;
- compilador experimental de un lenguaje pequeño a CASM.

Estas ampliaciones deben mantenerse opcionales. No son necesarias para que Ceres cumpla sus objetivos
principales.

## 14. Elementos que no deberían convertirse en prioridades

Para proteger el objetivo del proyecto, no conviene priorizar:

- compatibilidad con x86, ARM o RISC-V;
- ejecución de aplicaciones reales complejas;
- POSIX completo;
- seguridad industrial;
- JIT antes de tener una experiencia de depuración excelente;
- una GPU compleja;
- una jerarquía de procesos comparable a un sistema operativo real;
- optimizaciones que hagan más difícil explicar la ejecución.

Estas características podrían ser experimentos interesantes, pero no deben desplazar las mejoras que
aportan directamente aprendizaje y diversión.

## 15. Resultado objetivo

La versión ideal de Ceres para este propósito tendría estas propiedades:

- una persona puede entender la arquitectura leyendo la documentación;
- los faults explican qué ocurrió y cómo investigarlo;
- el ensamblador permite crear módulos y bibliotecas pequeñas sin sorpresas;
- las interrupciones y el MMIO tienen ejemplos claros;
- la entrada de terminal funciona de manera fiable y observable;
- el framebuffer permite dibujar juegos retro sencillos;
- el timer permite construir bucles de juego reproducibles;
- el debugger permite avanzar y retroceder por el programa;
- los ejemplos cubren tanto juegos como kernels;
- toda la plataforma sigue siendo pequeña, modificable y razonablemente fácil de entender.

Ese objetivo es suficientemente ambicioso para producir una plataforma rica, pero suficientemente
acotado para conservar la personalidad de Ceres: una máquina virtual educativa y jugable, no un
sistema operativo profesional ni una arquitectura comercial en miniatura.

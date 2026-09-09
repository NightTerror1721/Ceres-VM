# ceres-core - el contrato que comparte todo lo demás.
#
# Tres cosas, y ninguna de ellas ejecuta nada:
#
#   base/    tipos, u24/i24, fixed_vector, aserciones, utilidades de cadena
#   isa/     opcodes, el formato de instrucción, los bancos de registros, el desensamblador
#   format/  CRES (.cres), la información de depuración, el mapa de memoria de la plataforma
#
# El desensamblador está aquí y no en la VM a propósito: decodificar una palabra de 32 bits es
# función del juego de instrucciones, no del estado de una máquina en marcha, y así
# `ceres asm --listing` no tiene que enlazar el motor de ejecución.
#
# La información de depuración también, y ése es uno de los dos cortes de la separación: el
# ensamblador la produce y el depurador la consume, así que no puede vivir dentro de ninguno de
# los dos. El otro corte son las constantes del mapa de memoria, que el enlazador necesitaba y
# antes le obligaban a incluir vm::Memory entera.

ceres_add_library(core)
ceres_add_tests(core)

# ceres-asm - el ensamblador y el enlazador.
#
# Léxico, parser, macros, expresiones constantes, tabla de símbolos, emisor binario, objetos
# .cobj, archivos .car y el enlazador.
#
# Fíjate en lo que NO aparece en DEPENDS: ceres-vm. Ése es el resultado de los dos cortes. El
# ensamblador emite un .cres y necesita saber dónde empieza .text, pero no necesita una máquina
# - y hasta ahora incluía vm/memory.h y debug/debug_info.h para conseguirlo.
#
# Si alguien vuelve a añadir ceres::vm aquí, que sea una decisión y no un descuido.

ceres_add_library(asm DEPENDS core)
ceres_add_tests(asm)

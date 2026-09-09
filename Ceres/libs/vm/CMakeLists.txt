# ceres-vm - la máquina en marcha.
#
# Memoria, bancos de registros vivos, motor de ejecución, controlador de interrupciones, puertos
# de E/S y la BIOS. Lo que hace falta para cargar un .cres y ejecutarlo, y nada más: no ensambla,
# no sabe qué es un fichero fuente y no conoce ningún periférico concreto.
#
# Es la pieza que tendría sentido empotrar sola - en wasm, en un juego, en un panel web.

ceres_add_library(vm DEPENDS core)
ceres_add_tests(vm)

# ceres-debug - el depurador.
#
# Sesión de depuración, historial de ejecución hacia atrás, evaluador de expresiones, el JSON
# mínimo del protocolo, la CLI interactiva y el servidor NDJSON que habla con el editor.
#
# Depende de ceres-asm porque la sesión ensambla fuente sobre la marcha. Eso no es un ciclo: es
# el depurador estando por encima del ensamblador, que es donde le toca estar.
#
# Ojo: la información de depuración (el formato, las tablas de líneas y símbolos) NO está aquí.
# Vive en ceres-core, junto al resto de formatos.

ceres_add_library(debug DEPENDS core vm devices asm)
ceres_add_tests(debug)

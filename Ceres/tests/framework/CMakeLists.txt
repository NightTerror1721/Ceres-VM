# El andamio compartido de los tests: framework.h y el main() que ejecuta lo registrado.
#
# main.cpp se expone como fuente INTERFACE, no dentro de un archivo estático. Son dieciséis
# líneas que se recompilan por ejecutable, y a cambio no hay que razonar sobre si el enlazador
# extrae o no de un .lib el objeto que define main.
#
# Aquí no entra assemble_helper.h: ése arrastra el ensamblador entero y pertenece a tests/e2e.
# El andamio se queda sin dependencias para que una librería pueda probarse sola.

add_library(ceres_test_framework INTERFACE)
add_library(ceres::test_framework ALIAS ceres_test_framework)

target_include_directories(ceres_test_framework INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ceres_test_framework INTERFACE ceres::settings)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
	message(STATUS "ceres: falta tests/framework/main.cpp - no se construye ninguna suite")
	return()
endif()

add_library(ceres_test_main INTERFACE)
add_library(ceres::test_main ALIAS ceres_test_main)

target_sources(ceres_test_main INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/main.cpp>")
target_link_libraries(ceres_test_main INTERFACE ceres::test_framework)

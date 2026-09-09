# Los tests de extremo a extremo.
#
# Nueve de los quince ficheros de la suite actual pasan por assemble_helper.h: ensamblan fuente
# y ejecutan el resultado. test_encoding.cpp son 920 líneas que prueban la codificación de
# instrucciones... ensamblando texto.
#
# Son buenos tests, y no se trata de reescribirlos. Se trata de llamarlos por lo que son. Aquí
# viven los que necesitan la tubería entera; a libs/<x>/tests/ bajan sólo los que ya se sostienen
# con una librería sola: test_json y test_program_file a core, test_vm y test_devices a vm,
# test_expression y test_history a debug.

file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

if(NOT sources)
	return()
endif()

if(NOT TARGET ceres_test_main)
	message(STATUS "ceres: hay tests e2e, pero falta tests/framework/main.cpp - suite omitida")
	return()
endif()

add_executable(ceres_e2e_tests ${sources})

target_include_directories(ceres_e2e_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ceres_e2e_tests
	PRIVATE
		ceres::core
		ceres::vm
		ceres::devices
		ceres::asm
		ceres::debug
		ceres::test_main
		ceres::warnings)

set_target_properties(ceres_e2e_tests PROPERTIES FOLDER "tests")

add_test(NAME e2e COMMAND ceres_e2e_tests)

# ceres-cli - el producto: el ejecutable `ceres`.
#
# Los siete subcomandos (asm, run, disasm, profile, debug, link, ar) y el formateo de
# diagnósticos. Existe como proyecto aparte por una razón concreta: para que ninguna librería
# pueda depender de él. Todo lo que el CLI necesite de otra parte tiene que subir a una librería
# antes, y eso se nota al escribirlo.

file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

if(NOT sources)
	message(STATUS "ceres: apps/cli/src está vacío - el ejecutable no se construye todavía")
	return()
endif()

add_executable(ceres ${sources})

target_link_libraries(ceres
	PRIVATE
		ceres::core
		ceres::vm
		ceres::devices
		ceres::asm
		ceres::debug
		ceres::warnings)

set_target_properties(ceres PROPERTIES FOLDER "apps")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${sources})

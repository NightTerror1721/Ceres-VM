# Cómo se declara una parte de Ceres.
#
# Las cinco librerías son iguales por dentro, así que se declaran con una función en vez de con
# cinco CMakeLists.txt que se van desincronizando. Lo único que cambia entre ellas es el nombre
# y de quién dependen, y eso es exactamente lo que se pasa por parámetro.
#
# Sobre los globs: mientras dure la migración, los ficheros se descubren solos. Es la decisión
# correcta ahora - mover un .cpp de Ceres-ASM-old a su sitio no debería exigir además editar un
# CMakeLists - y la incorrecta cuando la migración acabe, porque un glob no puede avisar de que
# alguien se ha dejado un fichero fuera del árbol. CONFIGURE_DEPENDS hace que CMake vuelva a
# mirar en cada construcción, así que un fichero nuevo aparece sin reconfigurar a mano.
#
#   >>> Al terminar la migración: sustituir los globs por listas explícitas. <<<

include_guard(GLOBAL)

# El texto de la unidad de traducción marcador. Una librería estática necesita al menos un .cpp
# para que CMake sepa con qué enlazador tratarla, y durante la migración hay directorios src/
# todavía vacíos. En cuanto aparece un .cpp de verdad, esto deja de compilarse solo.
set(CERES_PLACEHOLDER_SOURCE [[
// Generado por CMake. No se versiona y no hace falta tocarlo.
//
// Esta librería aún no tiene fuentes propias: el código sigue en Ceres-ASM-old, pendiente de
// migrar. Un archivo estático vacío hace que el enlazador de MSVC avise (LNK4221) y que CMake
// no pueda deducir el lenguaje del target, así que aquí hay un símbolo y nada más. En cuanto
// haya un .cpp en src/, este fichero desaparece de la construcción sin que nadie lo borre.

namespace ceres::detail
{
	int placeholderTranslationUnit() noexcept;
	int placeholderTranslationUnit() noexcept { return 0; }
}
]])

# ceres_add_library(<nombre> [DEPENDS <nombre> ...])
#
# Crea ceres_<nombre> (estática) y el alias ceres::<nombre>. Las cabeceras de include/ son
# públicas y las de src/ no: la frontera deja de ser una convención y pasa a ser un error de
# compilación en cuanto alguien intenta cruzarla.
function(ceres_add_library name)
	cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "DEPENDS")

	set(root "${CMAKE_CURRENT_SOURCE_DIR}")
	set(target "ceres_${name}")

	file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${root}/src/*.cpp")
	file(GLOB_RECURSE private_headers CONFIGURE_DEPENDS "${root}/src/*.h")
	file(GLOB_RECURSE public_headers CONFIGURE_DEPENDS "${root}/include/*.h")

	set(compiled ${sources})
	if(NOT compiled)
		set(placeholder "${CMAKE_CURRENT_BINARY_DIR}/${target}_placeholder.cpp")
		if(NOT EXISTS "${placeholder}")
			file(WRITE "${placeholder}" "${CERES_PLACEHOLDER_SOURCE}")
		endif()
		set(compiled "${placeholder}")
	endif()

	add_library(${target} STATIC ${compiled} ${private_headers} ${public_headers})
	add_library(ceres::${name} ALIAS ${target})

	# BUILD_INTERFACE y no una ruta a secas: el día que esto se instale o se saque a su propio
	# repositorio, la ruta del árbol de fuentes no significa nada para quien lo consume.
	target_include_directories(${target}
		PUBLIC  "$<BUILD_INTERFACE:${root}/include>"
		PRIVATE "${root}/src")

	target_link_libraries(${target}
		PUBLIC  ceres::settings
		PRIVATE ceres::warnings)

	foreach(dependency IN LISTS ARG_DEPENDS)
		target_link_libraries(${target} PUBLIC ceres::${dependency})
	endforeach()

	set_target_properties(${target} PROPERTIES FOLDER "libs")

	# Que el explorador de soluciones de Visual Studio muestre el árbol real y no una lista plana.
	if(public_headers)
		source_group(TREE "${root}/include" PREFIX "include" FILES ${public_headers})
	endif()
	if(sources OR private_headers)
		source_group(TREE "${root}/src" PREFIX "src" FILES ${sources} ${private_headers})
	endif()

	list(LENGTH sources source_count)
	set_property(GLOBAL APPEND PROPERTY CERES_MIGRATION_STATUS "${name}=${source_count}")
endfunction()

# ceres_add_tests(<nombre>)
#
# La suite unitaria de una librería, si ya tiene ficheros en tests/. Se llama desde el
# CMakeLists de la propia librería, junto a su ceres_add_library().
function(ceres_add_tests name)
	if(NOT CERES_BUILD_TESTS)
		return()
	endif()

	file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
	if(NOT sources)
		return()
	endif()

	if(NOT TARGET ceres_test_main)
		message(STATUS "ceres: ${name} ya tiene tests, pero falta tests/framework/main.cpp - suite omitida")
		return()
	endif()

	set(target "ceres_${name}_tests")
	add_executable(${target} ${sources})
	target_link_libraries(${target} PRIVATE ceres::${name} ceres::test_main ceres::warnings)
	set_target_properties(${target} PROPERTIES FOLDER "tests")

	add_test(NAME ${name} COMMAND ${target})
endfunction()

# ceres_report_migration_status()
#
# Al configurar, un recuento de qué queda por mover. Barato de escribir y evita la pregunta de
# "¿esto ya está migrado?" en cada sesión.
function(ceres_report_migration_status)
	get_property(entries GLOBAL PROPERTY CERES_MIGRATION_STATUS)

	set(pending "")
	set(done "")
	foreach(entry IN LISTS entries)
		string(REPLACE "=" ";" parts "${entry}")
		list(GET parts 0 name)
		list(GET parts 1 count)
		if(count EQUAL 0)
			list(APPEND pending "${name}")
		else()
			list(APPEND done "${name} (${count})")
		endif()
	endforeach()

	message(STATUS "")
	message(STATUS "Ceres ${PROJECT_VERSION} - C++${CMAKE_CXX_STANDARD}, ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
	if(done)
		list(JOIN done ", " text)
		message(STATUS "  con fuentes : ${text}")
	endif()
	if(pending)
		list(JOIN pending ", " text)
		message(STATUS "  por migrar  : ${text}")
	endif()
	if(NOT TARGET ceres)
		message(STATUS "  ejecutable  : sin apps/cli/src/main.cpp todavía")
	endif()
	message(STATUS "")
endfunction()

# Ceres - la raíz de la construcción.
#
# Una sola verdad sobre qué ficheros forman el proyecto. Antes había tres que no se hablaban
# entre ellas (dos .vcxproj con la lista a mano, tests/build.sh con globs y una línea de g++ en
# el CI), y con seis proyectos eso serían dieciocho listas que mantener sincronizadas.
#
# El .vcxproj para Visual Studio pasa a estar generado por CMake, no escrito a mano:
#     cmake --preset msvc && cmake --build --preset msvc-debug
#
# Durante la migración los directorios están vacíos a propósito. Cada librería se construye
# igualmente - con una unidad de traducción marcador que desaparece sola en cuanto haya un .cpp
# de verdad en su src/ - para que el árbol configure y compile desde el primer día y se pueda ir
# moviendo código fichero a fichero sin dejar la rama rota por el camino.

cmake_minimum_required(VERSION 3.28)

project(Ceres
    VERSION      0.1.0
    DESCRIPTION  "Una máquina virtual de 32 bits y su ensamblador"
    HOMEPAGE_URL "https://github.com/Krampus1721/CeresASM"
    LANGUAGES    CXX)

# C++23 en todo el árbol, sin extensiones del compilador: lo que compile aquí compila con MSVC,
# GCC y Clang, que es justamente lo que descubrió las cuatro cabeceras que faltaban cuando MSVC
# era el único compilador que veía este código.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# Sólo tiene sentido cuando Ceres es el proyecto de arriba; si algún día se incluye desde otro
# árbol, quien mande decide dónde van los binarios y si se construyen los tests.
if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "cmake")
endif()

option(CERES_BUILD_TESTS      "Construir las suites de tests"          ${PROJECT_IS_TOP_LEVEL})
option(CERES_WARNINGS_AS_ERRORS "Tratar los avisos como errores"       OFF)

include(CeresSettings)
include(CeresLibrary)

if(CERES_BUILD_TESTS)
    enable_testing()
    # Antes que las librerías: ceres_add_tests() pregunta por el target del runner, y una
    # comprobación if(TARGET ...) se evalúa en el momento, no al generar.
    add_subdirectory(tests/framework)
endif()

# El orden es el del grafo de dependencias: nadie se declara antes que aquello de lo que depende.
add_subdirectory(libs/core)
add_subdirectory(libs/vm)
add_subdirectory(libs/devices)
add_subdirectory(libs/asm)
add_subdirectory(libs/debug)
add_subdirectory(apps/cli)

if(CERES_BUILD_TESTS)
    add_subdirectory(tests/e2e)
endif()

if(PROJECT_IS_TOP_LEVEL)
    ceres_report_migration_status()
endif()

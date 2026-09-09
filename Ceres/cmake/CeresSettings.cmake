# Dos targets INTERFACE que llevan todo lo que hoy está repartido entre los .vcxproj, el CI y
# tests/build.sh. Están separados a propósito:
#
#   ceres_settings  se enlaza PUBLIC. Es lo que cambia el lenguaje que ve quien nos incluye:
#                   el modo conforme, la macro CERES_DEBUG que lee common/config.h desde una
#                   cabecera, y las bibliotecas que hace falta enlazar en cada plataforma.
#
#   ceres_warnings  se enlaza PRIVATE. Los avisos son asunto de quien compila el fichero, no de
#                   quien lo incluye: si se propagaran, un aviso nuestro saldría en el código de
#                   otro y no habría forma de callarlo desde el sitio correcto.

include_guard(GLOBAL)
include(CheckLinkerFlag)

# ---------------------------------------------------------------------------- ceres_settings

add_library(ceres_settings INTERFACE)
add_library(ceres::settings ALIAS ceres_settings)

target_compile_features(ceres_settings INTERFACE cxx_std_23)

# CERES_DEBUG enciende las aserciones y el registro en common/config.h. Es una cabecera, así que
# la macro tiene que llegar también a quien nos incluya: de ahí que viva en el target PUBLIC y no
# en el de avisos. Antes la ponía a mano la configuración Debug del .vcxproj, y sólo esa.
target_compile_definitions(ceres_settings INTERFACE $<$<CONFIG:Debug>:CERES_DEBUG>)

if(MSVC)
    target_compile_options(ceres_settings INTERFACE
        /permissive-        # ConformanceMode del .vcxproj
        /utf-8              # las fuentes tienen acentos en los comentarios y literales
        /Zc:preprocessor    # el preprocesador conforme, no el heredado de VC6
        /Zc:__cplusplus     # sin esto __cplusplus miente y dice 199711L
        /EHsc)
endif()

# libstdc++ dejó fuera de la biblioteca principal parte de <print> y <stacktrace>. En MinGW hace
# falta siempre; en otras configuraciones de GCC depende de la versión, así que se pregunta al
# enlazador en vez de adivinar por la plataforma - que es lo que hacía tests/build.sh.
if(NOT MSVC)
    check_linker_flag(CXX "-lstdc++exp" CERES_HAS_LIBSTDCXXEXP)
    if(CERES_HAS_LIBSTDCXXEXP)
        target_link_libraries(ceres_settings INTERFACE stdc++exp)
    endif()
endif()

# ---------------------------------------------------------------------------- ceres_warnings

add_library(ceres_warnings INTERFACE)
add_library(ceres::warnings ALIAS ceres_warnings)

if(MSVC)
    # El .vcxproj estaba en Level3. /W4 es el nivel que se usa cuando el proyecto se toma en
    # serio los avisos, y ahora hay un sitio único donde subirlo o bajarlo.
    target_compile_options(ceres_warnings INTERFACE /W4)
else()
    # Exactamente los de tests/build.sh y los del CI: un parámetro sin usar es normal en un
    # manejador que cumple una firma, y avisar de eso sólo enseña a ignorar los avisos.
    target_compile_options(ceres_warnings INTERFACE -Wall -Wextra -Wno-unused-parameter)
endif()

if(CERES_WARNINGS_AS_ERRORS)
    if(MSVC)
        target_compile_options(ceres_warnings INTERFACE /WX)
    else()
        target_compile_options(ceres_warnings INTERFACE -Werror)
    endif()
endif()

# Compilación en paralelo con el generador de Visual Studio, que es lo que daba el -m de msbuild
# en el CI. Ninja ya reparte trabajo por su cuenta.
if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
    target_compile_options(ceres_warnings INTERFACE /MP)
endif()

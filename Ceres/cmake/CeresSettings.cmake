# Two INTERFACE targets that carry everything currently spread across the .vcxproj files, CI and
# tests/build.sh. They're kept separate on purpose:
#
#   ceres_settings  is linked PUBLIC. It's what changes the language seen by whoever includes us:
#                   conformance mode, the CERES_DEBUG macro read from common/config.h in a
#                   header, and the libraries that need linking on each platform.
#
#   ceres_warnings  is linked PRIVATE. Warnings are the business of whoever compiles the file,
#                   not whoever includes it: if they propagated, a warning of ours would show up
#                   in someone else's code with no way to silence it from the right place.

include_guard(GLOBAL)
include(CheckLinkerFlag)

# ---------------------------------------------------------------------------- ceres_settings

add_library(ceres_settings INTERFACE)
add_library(ceres::settings ALIAS ceres_settings)

target_compile_features(ceres_settings INTERFACE cxx_std_23)

# CERES_DEBUG turns on assertions and logging in common/config.h. It's a header, so the macro
# has to reach whoever includes us too: hence it lives on the PUBLIC target and not the warnings
# one. It used to be set by hand by the .vcxproj's Debug configuration, and only that one.
target_compile_definitions(ceres_settings INTERFACE $<$<CONFIG:Debug>:CERES_DEBUG>)

if(MSVC)
    target_compile_options(ceres_settings INTERFACE
        /permissive-        # ConformanceMode from the .vcxproj
        /utf-8              # source files may contain non-ASCII characters in comments and literals
        /Zc:preprocessor    # the conformant preprocessor, not the one inherited from VC6
        /Zc:__cplusplus     # without this __cplusplus lies and reports 199711L
        /EHsc)
endif()

# See CERES_ENABLE_ARCH_TUNING's definition in the root CMakeLists.txt for what this buys and why
# it defaults off. /Oi is requested explicitly too: /O2 alone doesn't reliably guarantee intrinsic
# substitution across MSVC versions the way GCC/Clang's -O2/-O3 already do.
if(CERES_ENABLE_ARCH_TUNING)
    if(MSVC)
        target_compile_options(ceres_settings INTERFACE /arch:AVX2 /Oi)
    else()
        target_compile_options(ceres_settings INTERFACE -mpopcnt -mbmi)
    endif()
elseif(MSVC)
    target_compile_options(ceres_settings INTERFACE /Oi)
endif()

# See CERES_PGO_GENERATE/CERES_PGO_USE's definition in the root CMakeLists.txt for the two-build
# workflow. Wired for GCC/Clang, where it was built and verified against libs/vm/benchmarks; MSVC's
# equivalent is /LTCG:PGInstrument (generate) then /LTCG:PGOptimize (use), which additionally needs
# CERES_ENABLE_IPO for the /LTCG it depends on - left as a FATAL_ERROR rather than guessed flags,
# since nothing in this tree can build with MSVC to verify the exact syntax before shipping it.
if(CERES_PGO_GENERATE AND CERES_PGO_USE)
    message(FATAL_ERROR "CERES_PGO_GENERATE and CERES_PGO_USE are mutually exclusive: record a profile first (CERES_PGO_GENERATE), run a representative workload, then rebuild against it (CERES_PGO_USE) - not both in the same build.")
endif()

if((CERES_PGO_GENERATE OR CERES_PGO_USE) AND MSVC)
    message(FATAL_ERROR "CERES_PGO_GENERATE/CERES_PGO_USE are not wired for MSVC yet - its /LTCG:PGInstrument + /LTCG:PGOptimize flags need verifying against an actual MSVC build before they're added here. Use the gcc or clang presets for PGO in the meantime.")
elseif(CERES_PGO_GENERATE)
    target_compile_options(ceres_settings INTERFACE -fprofile-generate=${CERES_PGO_DIR})
    target_link_options(ceres_settings INTERFACE -fprofile-generate=${CERES_PGO_DIR})
elseif(CERES_PGO_USE)
    target_compile_options(ceres_settings INTERFACE -fprofile-use=${CERES_PGO_DIR})
    target_link_options(ceres_settings INTERFACE -fprofile-use=${CERES_PGO_DIR})
    # -fprofile-correction: GCC-only. A profile trained on one binary and applied to a slightly
    # different rebuild (a touched comment, a different compiler point release) is otherwise a
    # hard error instead of a best-effort match - exactly the kind of drift a two-build workflow
    # invites. Clang tolerates a stale profile by default, so it needs no equivalent flag.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(ceres_settings INTERFACE -fprofile-correction)
    endif()
endif()

# libstdc++ left part of <print> and <stacktrace> out of the main library. On MinGW it's always
# needed; on other GCC configurations it depends on the version, so the linker is asked instead
# of guessing from the platform - which is what tests/build.sh used to do.
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
    # The .vcxproj was at Level3. /W4 is the level used when a project takes warnings
    # seriously, and now there's a single place to raise or lower it.
    target_compile_options(ceres_warnings INTERFACE /W4)
else()
    # Exactly what tests/build.sh and CI used: an unused parameter is normal in a handler that
    # fulfills a signature, and warning about that only teaches people to ignore warnings.
    target_compile_options(ceres_warnings INTERFACE -Wall -Wextra -Wno-unused-parameter)
endif()

if(CERES_WARNINGS_AS_ERRORS)
    if(MSVC)
        target_compile_options(ceres_warnings INTERFACE /WX)
    else()
        target_compile_options(ceres_warnings INTERFACE -Werror)
    endif()
endif()

# Parallel compilation with the Visual Studio generator, which is what msbuild's -m gave us
# in CI. Ninja already distributes work on its own.
if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
    target_compile_options(ceres_warnings INTERFACE /MP)
endif()

#!/bin/sh
# Builds and runs the test suite with GCC. MSVC users build the .vcxproj; this script exists so
# the suite can run on a second compiler, which is how the missing <limits> includes surfaced.
set -e

here=$(cd "$(dirname "$0")" && pwd)
src="$here/../Ceres-ASM/src"
out="${1:-$here/ceres-tests}"

CXX=${CXX:-g++}
CXXFLAGS=${CXXFLAGS:--std=c++23 -Wall -Wextra -Wno-unused-parameter}

# libstdc++'s std::print needs this on MinGW; harmless where it is not required.
LDLIBS=""
if "$CXX" -std=c++23 -x c++ -E - </dev/null >/dev/null 2>&1; then
	if "$CXX" --version | head -1 | grep -qi mingw || [ "${OS:-}" = "Windows_NT" ]; then
		LDLIBS="-lstdc++exp"
	fi
fi

echo "Building tests with $CXX..."
# shellcheck disable=SC2086
"$CXX" $CXXFLAGS -I"$src" -I"$here" -o "$out" \
	"$here/main.cpp" "$here/test_encoding.cpp" "$here/test_vm.cpp" "$here/test_pipeline.cpp" "$here/test_robustness.cpp" "$here/test_macros.cpp" "$here/test_modules.cpp" "$here/test_language.cpp" "$here/test_program_file.cpp" "$here/test_devices.cpp" "$here/test_debug_info.cpp" "$here/test_debugger.cpp" \
	"$src"/vm/*.cpp "$src"/assembler/*.cpp "$src"/debug/*.cpp $LDLIBS

echo "Running..."
"$out"

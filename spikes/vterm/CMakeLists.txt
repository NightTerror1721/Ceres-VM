# F0.4 spike: the virtual terminal's ANSI subset over real program output. Outside Ceres/ and outside the build.
#
#   cmake -S spikes/vterm -B spikes/vterm/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build spikes/vterm/build
cmake_minimum_required(VERSION 3.25)
project(ceres_vterm_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(vterm vterm.cpp)

# F0.4 spike: a 4-operator FM voice, its cost per sample and its determinism. Outside Ceres/ and outside the build.
#
#   cmake -S spikes/fm -B spikes/fm/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build spikes/fm/build
cmake_minimum_required(VERSION 3.25)
project(ceres_fm_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(fm fm.cpp)

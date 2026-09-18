# The SDL3 spike (Fase 0). A standalone windowed host that proves the cooperative step + poll +
# present loop and the SDL -> device input mapping, without touching any library. Built only when
# CERES_ENABLE_SDL is on; it will grow into the real backend in Fase 2.

add_executable(ceres_sdl_spike src/main.cpp)

file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/echo.casm" SPIKE_ECHO_SOURCE_PATH)

target_compile_definitions(ceres_sdl_spike PRIVATE
	SPIKE_ECHO_SOURCE="${SPIKE_ECHO_SOURCE_PATH}")

target_link_libraries(ceres_sdl_spike
	PRIVATE
		ceres::core
		ceres::vm
		ceres::devices
		ceres::asm
		ceres::warnings
		SDL3::SDL3)

set_target_properties(ceres_sdl_spike PROPERTIES FOLDER "apps")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES src/main.cpp)

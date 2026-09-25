# F0.3 spike: SDL_GPU on this machine. Outside Ceres/ and outside the normal build on purpose.
#
#   cmake -S spikes/sdl_gpu -B spikes/sdl_gpu/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#         [-DFETCHCONTENT_SOURCE_DIR_SDL3=<an SDL 3.4.16 checkout, e.g. Ceres/build/gcc-sdl-ipo/_deps/sdl3-src>]
#   cmake --build spikes/sdl_gpu/build
#
# See README.md for what it shows and what it found.
cmake_minimum_required(VERSION 3.25)
project(ceres_sdl_gpu_spike LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
	GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
	GIT_TAG        release-3.4.16
	GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(SDL3)

# dxc from the Windows SDK compiles HLSL to DXIL and, with -spirv, to SPIR-V.
file(GLOB sdkBins "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64")
list(SORT sdkBins ORDER DESCENDING)
find_program(DXC dxc HINTS $ENV{VULKAN_SDK}/Bin ${sdkBins} REQUIRED)

set(shader ${CMAKE_CURRENT_SOURCE_DIR}/quad.hlsl)
set(generated ${CMAKE_CURRENT_BINARY_DIR}/shaders)

# The Windows SDK's dxc lists -spirv but is built without the SPIR-V back end; the one in the Vulkan SDK or in a
# DirectXShaderCompiler release has it. Without it the spike runs on Direct3D 12 (DXIL) only.
execute_process(COMMAND ${DXC} -nologo -T ps_6_0 -E ps_main -spirv -Fo ${CMAKE_CURRENT_BINARY_DIR}/probe.spv ${shader}
	RESULT_VARIABLE spirvProbe OUTPUT_QUIET ERROR_QUIET)
if(spirvProbe EQUAL 0)
	set(targets dxil spirv)
else()
	set(targets dxil)
	message(STATUS "${DXC} cannot generate SPIR-V: the spike is built for Direct3D 12 only")
endif()

set(headers)
foreach(stage IN ITEMS vs ps)
	foreach(target IN LISTS targets)
		set(blob ${generated}/quad_${stage}.${target})
		set(header ${generated}/quad_${stage}_${target}.h)
		if(target STREQUAL "spirv")
			set(extra -spirv -fspv-target-env=vulkan1.0)
		else()
			set(extra)
		endif()
		add_custom_command(OUTPUT ${header}
			COMMAND ${CMAKE_COMMAND} -E make_directory ${generated}
			COMMAND ${DXC} -nologo -T ${stage}_6_0 -E ${stage}_main ${extra} -Fo ${blob} ${shader}
			COMMAND ${CMAKE_COMMAND} -DINPUT=${blob} -DOUTPUT=${header} -DNAME=quad_${stage}_${target} -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
			DEPENDS ${shader} ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
			VERBATIM)
		list(APPEND headers ${header})
	endforeach()
endforeach()

add_executable(sdl_gpu_spike main.cpp ${headers})
target_include_directories(sdl_gpu_spike PRIVATE ${generated})
if("spirv" IN_LIST targets)
	target_compile_definitions(sdl_gpu_spike PRIVATE SPIKE_HAS_SPIRV=1)
endif()
target_link_libraries(sdl_gpu_spike PRIVATE SDL3::SDL3)

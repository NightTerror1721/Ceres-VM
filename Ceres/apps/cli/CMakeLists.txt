# ceres-cli - the product: the `ceres` executable.
#
# The seven subcommands (asm, run, disasm, profile, debug, link, ar) and diagnostic formatting.
# It exists as a separate project for a specific reason: so that no library can depend on it.
# Anything the CLI needs from elsewhere has to move up into a library first, and that shows
# while writing it.

file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

if(NOT sources)
	message(STATUS "ceres: apps/cli/src is empty - the executable isn't built yet")
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

# The tests' shared scaffolding: framework.h and the main() that runs what's registered.
#
# main.cpp is exposed as an INTERFACE source, not inside a static file. It's sixteen lines that
# get recompiled per executable, and in exchange there's no need to reason about whether the
# linker pulls the object defining main out of a .lib or not.
#
# assemble_helper.h doesn't belong here: that one drags in the whole assembler and belongs to
# tests/e2e. The scaffolding stays free of dependencies so a library can be tested on its own.

add_library(ceres_test_framework INTERFACE)
add_library(ceres::test_framework ALIAS ceres_test_framework)

target_include_directories(ceres_test_framework INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ceres_test_framework INTERFACE ceres::settings)

# A glob with CONFIGURE_DEPENDS, not an if(EXISTS): the if is only evaluated at configure time,
# so migrating main.cpp would require reconfiguring by hand for the suites to show up - and until
# then test_*.cpp belongs to no target, which is exactly when IntelliSense goes quiet without
# saying why. With the glob, the next build notices and CMake reruns on its own.
file(GLOB runner CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")

if(NOT runner)
	message(STATUS "ceres: tests/framework/main.cpp is missing - no suite will be built")
	return()
endif()

add_library(ceres_test_main INTERFACE)
add_library(ceres::test_main ALIAS ceres_test_main)

target_sources(ceres_test_main INTERFACE "$<BUILD_INTERFACE:${runner}>")
target_link_libraries(ceres_test_main INTERFACE ceres::test_framework)

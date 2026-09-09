# The end-to-end tests.
#
# Nine of the current suite's fifteen files go through assemble_helper.h: they assemble source
# and run the result. test_encoding.cpp is 920 lines that test instruction encoding... by
# assembling text.
#
# They're good tests, and this isn't about rewriting them. It's about calling them what they are.
# The ones that need the whole pipeline live here; only the ones that already stand on a single
# library move down to libs/<x>/tests/: test_json and test_program_file to core, test_vm and
# test_devices to vm, test_expression and test_history to debug.

file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

if(NOT sources)
	return()
endif()

if(NOT TARGET ceres_test_main)
	message(STATUS "ceres: e2e tests exist, but tests/framework/main.cpp is missing - suite skipped")
	return()
endif()

add_executable(ceres_e2e_tests ${sources})

target_include_directories(ceres_e2e_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ceres_e2e_tests
	PRIVATE
		ceres::core
		ceres::vm
		ceres::devices
		ceres::asm
		ceres::debug
		ceres::test_main
		ceres::warnings)

set_target_properties(ceres_e2e_tests PROPERTIES FOLDER "tests")

add_test(NAME e2e COMMAND ceres_e2e_tests)

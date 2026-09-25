# One device per file, with its source and its test: a check on the tree, not on a binary.
add_test(NAME device_layout
	COMMAND ${CMAKE_COMMAND}
		-DDEVICES_DIR=${PROJECT_SOURCE_DIR}/libs/devices
		-P ${CMAKE_CURRENT_SOURCE_DIR}/check_device_layout.cmake)

# These tests execute the product binary. Library tests cover the VM and assembler; this suite
# freezes the process-facing contract: arguments, exit status, stdout and stderr.
if(NOT TARGET ceres)
	return()
endif()

add_test(NAME cli_contract
	COMMAND ${CMAKE_COMMAND}
		-DCERES_EXE=$<TARGET_FILE:ceres>
		-DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/check_only.casm
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cli_contract.cmake)

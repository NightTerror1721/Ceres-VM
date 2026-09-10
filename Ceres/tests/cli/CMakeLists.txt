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

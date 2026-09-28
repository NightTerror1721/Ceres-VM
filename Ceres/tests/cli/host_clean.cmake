# No program writes to the host's terminal (plan/v2 SPEC 1.4, F5.12): every example runs headless, with nothing
# typed, and leaves the process's stdout empty. A program that waits for input it never gets, or that never ends (a
# game), is stopped after a while; what it wrote until then is checked all the same.
if(NOT DEFINED CERES_EXE OR NOT DEFINED EXAMPLES_DIR)
	message(FATAL_ERROR "CERES_EXE and EXAMPLES_DIR are required")
endif()

file(GLOB programs "${EXAMPLES_DIR}/*.casm" "${EXAMPLES_DIR}/tutorial/*.casm")
list(LENGTH programs count)
if(count EQUAL 0)
	message(FATAL_ERROR "no examples under ${EXAMPLES_DIR}")
endif()

set(failures "")
foreach(program IN LISTS programs)
	execute_process(
		COMMAND "${CERES_EXE}" run "${program}" --headless --speed max
		WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
		TIMEOUT 3
		RESULT_VARIABLE result
		OUTPUT_VARIABLE stdout
		ERROR_VARIABLE stderr)
	if(NOT stdout STREQUAL "")
		string(APPEND failures "\n  ${program} wrote to the host's stdout: '${stdout}'")
	endif()
endforeach()
if(NOT failures STREQUAL "")
	message(FATAL_ERROR "programs reached the host's terminal:${failures}")
endif()
message(STATUS "${count} examples left the host's terminal alone")

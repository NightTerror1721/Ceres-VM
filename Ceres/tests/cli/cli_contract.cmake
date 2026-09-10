if(NOT DEFINED CERES_EXE OR NOT DEFINED FIXTURE)
	message(FATAL_ERROR "CERES_EXE and FIXTURE are required")
endif()

execute_process(
	COMMAND "${CERES_EXE}" asm "${FIXTURE}" --json
	RESULT_VARIABLE assemble_result
	OUTPUT_VARIABLE assemble_output
	ERROR_VARIABLE assemble_error)
if(NOT assemble_result EQUAL 0)
	message(FATAL_ERROR "ceres asm failed (${assemble_result}): ${assemble_error}")
endif()
if(NOT assemble_output STREQUAL "[]\n")
	message(FATAL_ERROR "ceres asm --json must emit an empty diagnostics array, got: ${assemble_output}")
endif()
if(NOT assemble_error STREQUAL "")
	message(FATAL_ERROR "ceres asm --json wrote unexpected stderr: ${assemble_error}")
endif()

execute_process(
	COMMAND "${CERES_EXE}" run "${FIXTURE}" --memory invalid
	RESULT_VARIABLE memory_result
	OUTPUT_VARIABLE memory_output
	ERROR_VARIABLE memory_error)
if(NOT memory_result EQUAL 2)
	message(FATAL_ERROR "invalid --memory must return 2, got ${memory_result}")
endif()
if(NOT memory_output STREQUAL "")
	message(FATAL_ERROR "invalid --memory wrote unexpected stdout: ${memory_output}")
endif()
string(FIND "${memory_error}" "--memory needs a positive integer number of bytes" memory_message)
if(memory_message EQUAL -1)
	message(FATAL_ERROR "invalid --memory did not report its error: ${memory_error}")
endif()

# The same program at two speeds (plan/v2 F2.9): --speed max runs as fast as the host can, --speed 4x keeps four
# machine seconds to one of the host's. What the machine sees - its output, its cycle count at the end - must
# be the same either way; only the time the run takes may differ.
if(NOT DEFINED CERES_EXE OR NOT DEFINED FIXTURE)
	message(FATAL_ERROR "CERES_EXE and FIXTURE are required")
endif()

set(outputs "")
foreach(speed max 4x)
	execute_process(
		COMMAND ${CMAKE_COMMAND} -E env CERES_HEADLESS=1 "${CERES_EXE}" run "${FIXTURE}" --speed ${speed}
		RESULT_VARIABLE result
		OUTPUT_VARIABLE output
		ERROR_VARIABLE error)
	if(NOT result EQUAL 0)
		message(FATAL_ERROR "ceres run --speed ${speed} failed (${result}): ${error}")
	endif()
	if(NOT output MATCHES "^\\.\\.\\.[0-9A-F]+\n$")
		message(FATAL_ERROR "ceres run --speed ${speed} printed an unexpected line: '${output}'")
	endif()
	list(APPEND outputs "${output}")
endforeach()

list(GET outputs 0 at_max)
list(GET outputs 1 at_4x)
if(NOT at_max STREQUAL at_4x)
	message(FATAL_ERROR "the machine saw different things at different speeds: max '${at_max}', 4x '${at_4x}'")
endif()

# A program's output never reaches the host's terminal (plan/v2 SPEC 1.4, F5.7): run headless, a program that
# writes to both of its streams leaves the process's stdout and stderr empty, and --transcript has every byte,
# the error stream's between ESC[E and ESC[e.
if(NOT DEFINED CERES_EXE OR NOT DEFINED FIXTURE)
	message(FATAL_ERROR "CERES_EXE and FIXTURE are required")
endif()

set(transcript "${CMAKE_CURRENT_BINARY_DIR}/headless_output.txt")
file(REMOVE "${transcript}")
execute_process(
	COMMAND "${CERES_EXE}" run "${FIXTURE}" --headless --transcript "${transcript}"
	RESULT_VARIABLE result
	OUTPUT_VARIABLE stdout
	ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "ceres run failed (${result}): ${stderr}")
endif()
if(NOT stdout STREQUAL "")
	message(FATAL_ERROR "the program's output reached the host's stdout: '${stdout}'")
endif()
if(NOT stderr STREQUAL "")
	message(FATAL_ERROR "the program's output reached the host's stderr: '${stderr}'")
endif()
string(ASCII 27 esc)
file(READ "${transcript}" got)
set(expected "out${esc}[Eerr${esc}[e!")
if(NOT got STREQUAL expected)
	message(FATAL_ERROR "unexpected transcript: '${got}'")
endif()

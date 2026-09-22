# Runs `RevStudio --selftest` and checks the outcome from its own report file rather than from captured stdout.
#
# Why: on Windows RevStudio is a GUI-subsystem program, and whether its stdout reaches a parent that captures it is
# platform detail we do not want the tests to depend on. The report file is written by the program itself.
#
#   cmake -DEXE=<RevStudio> -DRB=<backend> -DREPORT=<file> -DEXPECT=<regex> [-DEXPECT_FAILURE=ON] -P run_selftest.cmake

foreach(required EXE RB REPORT EXPECT)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "run_selftest.cmake: -D${required}=... is required")
  endif()
endforeach()

file(REMOVE "${REPORT}")
execute_process(
  COMMAND "${EXE}" --selftest --rb "${RB}" --report "${REPORT}"
  RESULT_VARIABLE exit_code
  OUTPUT_VARIABLE stdout_text
  ERROR_VARIABLE stderr_text
  TIMEOUT 60)

if(EXISTS "${REPORT}")
  file(READ "${REPORT}" report)
else()
  set(report "")
endif()

message("---- self-test report (exit code ${exit_code}) ----\n${report}")

if(NOT report)
  message(FATAL_ERROR "the self-test wrote no report (stderr: ${stderr_text})")
endif()

if(EXPECT_FAILURE)
  if(exit_code EQUAL 0)
    message(FATAL_ERROR "the self-test was expected to fail but exited with 0")
  endif()
else()
  if(NOT exit_code EQUAL 0)
    message(FATAL_ERROR "the self-test failed (exit code ${exit_code})")
  endif()
endif()

if(NOT report MATCHES "${EXPECT}")
  message(FATAL_ERROR "the report does not match '${EXPECT}'")
endif()

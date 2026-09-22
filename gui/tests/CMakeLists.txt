# QtTest suites, the mock backend, and end-to-end runs of `RevStudio --selftest`.
# All of them run headless (QT_QPA_PLATFORM=offscreen), so CI needs no display.

# The mock backend: a stand-in for `rb --server`. A plain console program (QtCore only).
#
# RUNTIME_OUTPUT_DIRECTORY is pinned explicitly here because qt_standard_project_setup() sets the *global*
# CMAKE_RUNTIME_OUTPUT_DIRECTORY to the top build dir ON WINDOWS ONLY, if it isn't already set
# (https://doc.qt.io/qt-6/qt-standard-project-setup.html) -- so every add_executable() in this file would otherwise
# land in gui/build/<preset>/mock-rb.exe on Windows but gui/build/<preset>/tests/mock-rb on Linux/macOS. CMake's own
# test runner (ctest) never notices, since add_test()/$<TARGET_FILE:...> resolve the real location either way -- but a
# workflow step that needs a literal path to hand to a script does notice (gui-build.yml's Windows smoke test: it
# passed --backend gui/build/ci-windows-release/tests/mock-rb.exe, which didn't exist because the file was actually at
# gui/build/ci-windows-release/mock-rb.exe). Pinning the directory here keeps "gui/build/<preset>/tests/..." true and
# documented on every platform instead of hardcoding a second, Windows-only path somewhere else.
add_executable(mock-rb mock_rb.cpp)
target_link_libraries(mock-rb PRIVATE Qt6::Core)
set_target_properties(mock-rb PROPERTIES
  WIN32_EXECUTABLE OFF
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

function(revstudio_add_qtest name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE revstudio_core Qt6::Test revstudio_warnings)
  target_compile_definitions(${name} PRIVATE
    "MOCK_RB_PATH=\"$<TARGET_FILE:mock-rb>\""
    "TRANSCRIPT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/transcripts\"")
  add_dependencies(${name} mock-rb)
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})  # see the note above mock-rb
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
endfunction()

revstudio_add_qtest(tst_protocolcodec)
revstudio_add_qtest(tst_backendlocator)
revstudio_add_qtest(tst_session)

# End to end: the real executable and the real start-up path against the mock. The outcome is read from the report
# file the program writes (see run_selftest.cmake), not from captured stdout.
add_dependencies(RevStudio mock-rb)

function(revstudio_add_selftest name expect)
  cmake_parse_arguments(ARG "EXPECT_FAILURE" "RB" "ENVIRONMENT" ${ARGN})
  if(NOT ARG_RB)
    set(ARG_RB $<TARGET_FILE:mock-rb>)
  endif()
  set(expect_failure_flag "")
  if(ARG_EXPECT_FAILURE)
    set(expect_failure_flag -DEXPECT_FAILURE=ON)
  endif()
  add_test(NAME ${name}
           COMMAND ${CMAKE_COMMAND}
                   -DEXE=$<TARGET_FILE:RevStudio> -DRB=${ARG_RB}
                   -DREPORT=${CMAKE_CURRENT_BINARY_DIR}/${name}.report.txt
                   "-DEXPECT=${expect}" ${expect_failure_flag}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/run_selftest.cmake)
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${ARG_ENVIRONMENT}"
    TIMEOUT 90)
endfunction()

revstudio_add_selftest(selftest_mock "icon: ok.*handshake: ok.*ping: ok.*submit: ok.*variables: ok.*shutdown: ok.*selftest: PASSED")

# A backend like the phase 0 stub: the submit step is skipped, everything else still has to pass.
revstudio_add_selftest(selftest_mock_stub "submit: skipped.*shutdown: ok.*selftest: PASSED" ENVIRONMENT "MOCK_RB_OPTIONS=--stub")

# A missing backend must fail loudly and say where it looked.
revstudio_add_selftest(selftest_no_backend "locate: FAILED: no backend found; tried: .*does-not-exist.*selftest: FAILED"
                       EXPECT_FAILURE RB ${CMAKE_CURRENT_BINARY_DIR}/does-not-exist)

# --version and --help print to stdout and need no display. Not run on Windows, where RevStudio is a GUI-subsystem
# program and capturing its stdout is not something the tests should depend on.
if(NOT WIN32)
  add_test(NAME version COMMAND RevStudio --version)
  set_tests_properties(version PROPERTIES PASS_REGULAR_EXPRESSION "RevStudio [0-9]+\\.[0-9]+\\.[0-9]+ \\(Qt [0-9]+\\.[0-9]+")
  add_test(NAME help COMMAND RevStudio --help)
  set_tests_properties(help PROPERTIES PASS_REGULAR_EXPRESSION "--selftest")
endif()

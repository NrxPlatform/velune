include_guard(GLOBAL)

find_package(Python3 COMPONENTS Interpreter QUIET)

if(NOT Python3_Interpreter_FOUND)
    message(STATUS "Python3 not found: Test262 helper targets are unavailable")
    return()
endif()

# Full pinned upstream checkout. It remains ignored by Git and supplies the
# authoritative Test262 harness plus an optional full-suite execution source.
set(JSENGINE_TEST262_ROOT "${CMAKE_SOURCE_DIR}/third_party/test262" CACHE PATH "Path to the pinned upstream Test262 checkout")

# Curated, committed conformance corpus. This is the default Test262 target used
# during phase-by-phase semantic closure.
set(JSENGINE_TEST262_TEST_ROOT "${CMAKE_SOURCE_DIR}/tests/test262/test" CACHE PATH "Path to Velune's committed Test262 test corpus")
set(JSENGINE_TEST262_HARNESS_ROOT "${JSENGINE_TEST262_ROOT}/harness" CACHE PATH "Path to the authoritative Test262 harness")
set(JSENGINE_TEST262_TIMEOUT "5" CACHE STRING "Per-Test262-test timeout in seconds")
set(JSENGINE_TEST262_JOBS "8" CACHE STRING "Parallel Test262 jobs")

add_custom_target(test262-setup
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/test262/setup.py"
            --destination "${JSENGINE_TEST262_ROOT}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    USES_TERMINAL
    COMMENT "Fetching pinned Test262 revision"
)

if(TARGET js)
    add_custom_target(test262-smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/test262/run.py"
                --engine "$<TARGET_FILE:js>"
                --test-root "${JSENGINE_TEST262_TEST_ROOT}"
                --harness-root "${JSENGINE_TEST262_HARNESS_ROOT}"
                --timeout "${JSENGINE_TEST262_TIMEOUT}"
                --jobs "${JSENGINE_TEST262_JOBS}"
                --limit 100
        DEPENDS js
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        USES_TERMINAL
        COMMENT "Running a 100-file smoke test from Velune's committed Test262 corpus"
    )

    add_custom_target(test262
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/test262/run.py"
                --engine "$<TARGET_FILE:js>"
                --test-root "${JSENGINE_TEST262_TEST_ROOT}"
                --harness-root "${JSENGINE_TEST262_HARNESS_ROOT}"
                --timeout "${JSENGINE_TEST262_TIMEOUT}"
                --jobs "${JSENGINE_TEST262_JOBS}"
        DEPENDS js
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        USES_TERMINAL
        COMMENT "Running Velune's committed Test262 conformance corpus"
    )

    add_custom_target(test262-upstream
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/test262/run.py"
                --engine "$<TARGET_FILE:js>"
                --test262-root "${JSENGINE_TEST262_ROOT}"
                --timeout "${JSENGINE_TEST262_TIMEOUT}"
                --jobs "${JSENGINE_TEST262_JOBS}"
        DEPENDS js
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        USES_TERMINAL
        COMMENT "Running the full pinned upstream Test262 suite"
    )
endif()

if(BUILD_TESTING OR JSENGINE_BUILD_TESTS)
    add_test(
        NAME test262.runner.unit
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test262/runner/test_runner.py"
    )
endif()

execute_process(COMMAND "${STEAMFIX_INVENTORY}" RESULT_VARIABLE inventory_status)
if(NOT "${inventory_status}" STREQUAL "0")
    message(FATAL_ERROR "Coverage inventory failed: ${inventory_status}")
endif()

execute_process(COMMAND "${STEAMFIX_CTEST}" --test-dir "${STEAMFIX_BINARY_DIR}"
    -C Debug --output-on-failure --no-tests=error RESULT_VARIABLE test_status)
if(NOT "${test_status}" STREQUAL "0")
    message(FATAL_ERROR "Coverage tests failed: ${test_status}")
endif()

# Runs every example executable and fails if any of them fails.
# Copyright 2026 Summon Software Labs. Apache License 2.0.

if(NOT DEFINED PEF_BIN_DIR)
    message(FATAL_ERROR "PEF_BIN_DIR is required")
endif()

set(example_names
    pef_example_create_persist
    pef_example_checkpoint_resume
    pef_example_worker_reincarnation
    pef_example_stale_continuation
    pef_example_replay_safe
    pef_example_ambiguous_nonrepeatable
    pef_example_coordinator_restart
    pef_example_installed_consumer)

foreach(example IN LISTS example_names)
    set(executable "${PEF_BIN_DIR}/${example}${CMAKE_EXECUTABLE_SUFFIX}")
    if(NOT EXISTS "${executable}")
        message(FATAL_ERROR "example executable is missing: ${executable}")
    endif()
    message(STATUS "running ${example}")
    execute_process(
        COMMAND "${executable}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${example} failed with ${result}\n${output}\n${error}")
    endif()
endforeach()

message(STATUS "all examples completed successfully")

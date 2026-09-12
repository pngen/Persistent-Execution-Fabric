cmake_minimum_required(VERSION 3.20)

project(PersistentExecutionFabric
    VERSION 1.0.0
    DESCRIPTION "Vendor-neutral C++20 runtime for durable execution identity, resumable progress, checkpoints, replay, recovery, fencing, and generation-bound continuation"
    HOMEPAGE_URL "https://github.com/pngen/Persistent-Execution-Fabric"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PEF_BUILD_TOOLS "Build the coordinator, worker, and CLI executables" ON)
option(PEF_BUILD_TESTS "Build the test suites" ON)
option(PEF_BUILD_EXAMPLES "Build the runnable examples" ON)
option(PEF_BUILD_BENCHMARKS "Build the benchmark executables" ON)
option(PEF_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(PEF_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(PEF_ENABLE_CUDA "Build the optional CUDA-bound continuation proof" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(PEF_IS_TOP_LEVEL ON)
else()
    set(PEF_IS_TOP_LEVEL OFF)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------
# Strict first-party settings are applied per target so that no internal
# helper target leaks into the installed package.
function(pef_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /Zc:preprocessor
            /external:anglebrackets /external:W0)
        if(PEF_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
        if(PEF_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
    if(PEF_ENABLE_ASAN)
        if(MSVC)
            target_compile_options(${target} PRIVATE /fsanitize=address)
        else()
            target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
set(PEF_CORE_SOURCES
    src/version.cpp
    src/hash.cpp
    src/status.cpp
    src/bytes.cpp
    src/lifecycle.cpp
    src/side_effect.cpp
    src/policy.cpp
    src/records.cpp
    src/persistence.cpp
    src/recovery.cpp
    src/audit.cpp
    src/runtime.cpp
    src/protocol.cpp
    src/net.cpp
    src/server.cpp
    src/client.cpp
    src/explain.cpp
)

add_library(pef ${PEF_CORE_SOURCES})
add_library(PersistentExecutionFabric::pef ALIAS pef)

target_include_directories(pef
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(pef PUBLIC cxx_std_20)
pef_apply_warnings(pef)
target_link_libraries(pef PUBLIC $<$<CXX_COMPILER_ID:MSVC>:ws2_32>)

set_target_properties(pef PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

# ---------------------------------------------------------------------------
# Install rules
# ---------------------------------------------------------------------------
install(TARGETS pef
    EXPORT PersistentExecutionFabricTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/pef
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp")

install(EXPORT PersistentExecutionFabricTargets
    FILE PersistentExecutionFabricTargets.cmake
    NAMESPACE PersistentExecutionFabric::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PersistentExecutionFabric)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PersistentExecutionFabricConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/PersistentExecutionFabricConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PersistentExecutionFabric)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/PersistentExecutionFabricConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/PersistentExecutionFabricConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/PersistentExecutionFabricConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PersistentExecutionFabric)

install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/PersistentExecutionFabric)

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(PEF_BUILD_EXAMPLES)
    function(pef_add_example target_name source)
        add_executable(${target_name} ${source})
        target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples)
        target_link_libraries(${target_name} PRIVATE pef)
        pef_apply_warnings(${target_name})
    endfunction()

    pef_add_example(pef_example_create_persist examples/ex_create_persist.cpp)
    pef_add_example(pef_example_checkpoint_resume examples/ex_checkpoint_resume.cpp)
    pef_add_example(pef_example_worker_reincarnation examples/ex_worker_reincarnation.cpp)
    pef_add_example(pef_example_stale_continuation examples/ex_stale_continuation.cpp)
    pef_add_example(pef_example_replay_safe examples/ex_replay_safe.cpp)
    pef_add_example(pef_example_ambiguous_nonrepeatable examples/ex_ambiguous_nonrepeatable.cpp)
    pef_add_example(pef_example_coordinator_restart examples/ex_coordinator_restart.cpp)
    pef_add_example(pef_example_installed_consumer examples/ex_installed_consumer.cpp)

    if(PEF_BUILD_TESTS)
        add_test(NAME pef_examples COMMAND ${CMAKE_COMMAND}
            -DPEF_BIN_DIR=$<TARGET_FILE_DIR:pef_example_create_persist>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunExamples.cmake)
        set_tests_properties(pef_examples PROPERTIES TIMEOUT 300)
    endif()
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(PEF_BUILD_BENCHMARKS)
    add_executable(pef_benchmarks benchmarks/bench_core.cpp)
    target_link_libraries(pef_benchmarks PRIVATE pef)
    pef_apply_warnings(pef_benchmarks)
endif()

# ---------------------------------------------------------------------------
# Optional hardware-bound continuation proof
#
# The CUDA driver is loaded dynamically at run time, so enabling this needs no
# CUDA toolkit and no CUDA build support. When no driver or device is present
# the proof reports UNSUPPORTED and exits successfully without claiming
# hardware evidence.
# ---------------------------------------------------------------------------
if(PEF_ENABLE_CUDA)
    add_executable(pef_cuda_continuation
        cuda/pef_cuda_continuation.cpp
        cuda/cuda_driver.cpp)
    target_include_directories(pef_cuda_continuation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cuda)
    target_link_libraries(pef_cuda_continuation PRIVATE pef)
    if(WIN32)
        target_link_libraries(pef_cuda_continuation PRIVATE ${CMAKE_DL_LIBS})
    else()
        target_link_libraries(pef_cuda_continuation PRIVATE dl)
    endif()
    pef_apply_warnings(pef_cuda_continuation)
    if(PEF_BUILD_TESTS)
        add_test(NAME pef_cuda_continuation COMMAND pef_cuda_continuation)
        set_tests_properties(pef_cuda_continuation PROPERTIES TIMEOUT 300)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PEF_BUILD_TESTS)
    enable_testing()

    add_library(pef_test_support STATIC tests/test_framework.cpp)
    target_include_directories(pef_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(pef_test_support PUBLIC pef)
    pef_apply_warnings(pef_test_support)

    function(pef_add_test target_name)
        add_executable(${target_name} tests/test_main.cpp ${ARGN})
        target_link_libraries(${target_name} PRIVATE pef_test_support)
        pef_apply_warnings(${target_name})
        add_test(NAME ${target_name} COMMAND ${target_name})
    endfunction()

    add_library(pef_process_support STATIC tests/process_helper.cpp)
    target_include_directories(pef_process_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(pef_process_support PUBLIC pef)
    pef_apply_warnings(pef_process_support)

    function(pef_add_multiprocess_test target_name)
        add_executable(${target_name} tests/test_main.cpp ${ARGN})
        target_link_libraries(${target_name} PRIVATE pef_test_support pef_process_support)
        target_compile_definitions(${target_name} PRIVATE
            PEF_COORDINATOR_EXE="$<TARGET_FILE:pef_coordinator>"
            PEF_WORKER_EXE="$<TARGET_FILE:pef_worker>"
            PEF_CLI_EXE="$<TARGET_FILE:pef_cli>")
        add_dependencies(${target_name} pef_coordinator pef_worker pef_cli)
        pef_apply_warnings(${target_name})
        add_test(NAME ${target_name} COMMAND ${target_name})
        set_tests_properties(${target_name} PROPERTIES TIMEOUT 900)
    endfunction()

    pef_add_test(pef_tests_core tests/test_core.cpp)
    pef_add_test(pef_tests_runtime tests/test_runtime.cpp)
    pef_add_test(pef_tests_adversarial tests/test_adversarial.cpp)
    pef_add_test(pef_tests_concurrency tests/test_concurrency.cpp)
    pef_add_test(pef_tests_property tests/test_property.cpp)
    pef_add_test(pef_tests_scale tests/test_scale.cpp)

    if(PEF_BUILD_TOOLS)
        pef_add_multiprocess_test(pef_tests_multiprocess tests/test_multiprocess.cpp)
    endif()
endif()

# ---------------------------------------------------------------------------
# Control-plane executables
# ---------------------------------------------------------------------------
if(PEF_BUILD_TOOLS)
    function(pef_add_tool target_name source)
        add_executable(${target_name} ${source})
        target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/apps)
        target_link_libraries(${target_name} PRIVATE pef)
        pef_apply_warnings(${target_name})
        install(TARGETS ${target_name} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endfunction()

    pef_add_tool(pef_coordinator apps/pef_coordinator/main.cpp)
    pef_add_tool(pef_worker apps/pef_worker/main.cpp)
    pef_add_tool(pef_cli apps/pef_cli/main.cpp)
endif()

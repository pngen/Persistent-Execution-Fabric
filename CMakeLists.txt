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

    pef_add_test(pef_tests_core tests/test_core.cpp)
    pef_add_test(pef_tests_runtime tests/test_runtime.cpp)
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

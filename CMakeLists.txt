# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# Top-level CMake for the ddbsh reimplementation (D14).
#
# Three targets from one build:
#   ddbsh          — the engine library (SDK-free public API; no terminal)
#   dynamodb-shell — the thin CLI (readline + AWS SDK + AwsDDBClient)
#   ddbsh-tests    — native GoogleTest unit tests + the boundary check
#
# The compiler is NOT hardcoded (choose via CMakePresets or -DCMAKE_CXX_COMPILER).
# C++23 is required for std::expected (D12).
#
# NOTE (2a, "alongside" strategy): the original C++11 golden build still lives
# at ddbsh/CMakeLists.txt and is the fidelity oracle exercised by
# devtools/run-tests. It is intentionally NOT wired into this build yet; the
# original sources are ported into src/ per sub-phase (2b-2e). This preserves
# the golden .exp regression contract while the reimplementation grows. See the
# discussion entry for the Q3-reorg deviation rationale.

cmake_minimum_required(VERSION 3.21)

project(ddbsh
        VERSION 0.9.1
        DESCRIPTION "A DynamoDB interactive shell (reimplementation)"
        HOMEPAGE_URL "https://www.ddbsh.org/"
        LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(DDBSH_BUILD_TESTS "Build the native unit tests" ON)
option(DDBSH_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

# Warnings-as-errors, narrowly scoped to our own targets (not third-party).
add_library(ddbsh_warnings INTERFACE)
if(MSVC)
    target_compile_options(ddbsh_warnings INTERFACE /W4)
    if(DDBSH_WARNINGS_AS_ERRORS)
        target_compile_options(ddbsh_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(ddbsh_warnings INTERFACE -Wall -Wextra)
    if(DDBSH_WARNINGS_AS_ERRORS)
        target_compile_options(ddbsh_warnings INTERFACE -Werror)
    endif()
endif()

# --- flex + bison (reentrant scanner + C++ lalr1.cc parser, D3) -------------
find_package(BISON 3.7 REQUIRED)
find_package(FLEX REQUIRED)

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/parser")
file(MAKE_DIRECTORY "${GEN_DIR}")

BISON_TARGET(ddbsh_parser
             src/parser/grammar.yy
             "${GEN_DIR}/grammar.cpp"
             DEFINES_FILE "${GEN_DIR}/grammar.hpp")
FLEX_TARGET(ddbsh_scanner
            src/parser/lexer.l
            "${GEN_DIR}/lexer.cpp"
            DEFINES_FILE "${GEN_DIR}/lexer.hpp")
ADD_FLEX_BISON_DEPENDENCY(ddbsh_scanner ddbsh_parser)

# --- engine library ----------------------------------------------------------
add_library(ddbsh
            src/session.cpp
            src/version.cpp
            src/render/renderer.cpp
            src/parser/driver.cpp
            ${BISON_ddbsh_parser_OUTPUTS}
            ${FLEX_ddbsh_scanner_OUTPUTS})
add_library(ddbsh::ddbsh ALIAS ddbsh)

# project() leaves PROJECT_VERSION_TWEAK empty for a three-part version; default
# it to 0 so version_config.hpp gets a valid integer (0.9.1 -> "0.9.1").
if(NOT DEFINED PROJECT_VERSION_TWEAK OR PROJECT_VERSION_TWEAK STREQUAL "")
    set(PROJECT_VERSION_TWEAK 0)
endif()

configure_file(src/version_config.hpp.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/version_config.hpp" @ONLY)

target_include_directories(ddbsh
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_BINARY_DIR}/generated"
            "${GEN_DIR}")
target_link_libraries(ddbsh PRIVATE ddbsh_warnings)
set_target_properties(ddbsh PROPERTIES OUTPUT_NAME ddbsh)

# The generated flex/bison sources are not ours to lint; suppress -Werror on
# just those translation units.
set_source_files_properties(
    "${GEN_DIR}/grammar.cpp" "${GEN_DIR}/lexer.cpp"
    PROPERTIES COMPILE_OPTIONS "-Wno-error")

# --- CLI binary --------------------------------------------------------------
find_package(AWSSDK REQUIRED COMPONENTS dynamodb iam sts)
find_package(ZLIB REQUIRED)

add_executable(dynamodb-shell
               cli/main.cpp
               cli/aws_ddb_client.cpp)
target_include_directories(dynamodb-shell PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/cli")
target_link_libraries(dynamodb-shell
    PRIVATE ddbsh::ddbsh ddbsh_warnings readline ${AWSSDK_LINK_LIBRARIES})

# --- 2a readiness-gate smoke program (temporary, D-Gate) ---------------------
# Gated OFF by default; a throwaway diagnostic removed once the gate is cleared
# and the reorg proceeds. Proves engine pipeline + version wiring and real
# backend reachability via the AwsDDBClient seam.
option(DDBSH_BUILD_SMOKE_GATE "Build the 2a readiness-gate smoke program" OFF)
if(DDBSH_BUILD_SMOKE_GATE)
    add_executable(ddbsh-smoke-gate
                   cli/smoke_gate.cpp
                   cli/aws_ddb_client.cpp)
    target_include_directories(ddbsh-smoke-gate PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/cli")
    target_link_libraries(ddbsh-smoke-gate
        PRIVATE ddbsh::ddbsh ddbsh_warnings ${AWSSDK_LINK_LIBRARIES})
endif()

# --- tests -------------------------------------------------------------------
if(DDBSH_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(ddbsh-tests
                   tests/walking_path_test.cpp
                   tests/reentrancy_test.cpp)
    target_include_directories(ddbsh-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(ddbsh-tests
        PRIVATE ddbsh::ddbsh ddbsh_warnings GTest::gtest_main)

    include(GoogleTest)
    # PRE_TEST discovery enumerates tests at ctest time (not build time), so the
    # binary is not executed during the build. This matters for the sanitizer
    # preset, where the ASan runtime (libasan) is not on the default loader path
    # at build time; ctest runs with the proper environment.
    gtest_discover_tests(ddbsh-tests DISCOVERY_MODE PRE_TEST)

    add_test(NAME engine_boundary_check
             COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/boundary_check.sh"
                     "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

install(TARGETS ddbsh dynamodb-shell
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
install(DIRECTORY include/ddbsh DESTINATION include)

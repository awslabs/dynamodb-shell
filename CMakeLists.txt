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
# NOTE (2a reorg, P2a-01 D-Q3): the reimplementation now owns the top-level
# tree (include/ src/ cli/ tests/). The original C++11 golden build — the
# fidelity oracle exercised by devtools/run-tests against real DynamoDB — was
# relocated to oracle/CMakeLists.txt and is intentionally NOT wired into this
# build (it uses the retired TEST_HARNESS allocator and C++11 idioms that would
# not survive this tree's -Werror). The original sources are ported into src/
# per sub-phase (2b-2e); until a family is ported, the oracle remains the
# regression contract. See the P2a-01 discussion for the reorg rationale.

cmake_minimum_required(VERSION 3.21)

project(ddbsh
        VERSION 0.9.2
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
            src/statement.cpp
            src/version.cpp
            src/render/renderer.cpp
            src/parser/driver.cpp
            src/parser/lex_debug.cpp
            src/expressions/symbol_table.cpp
            src/expressions/attribute_serialize.cpp
            src/planner/get_key_schema.cpp
            src/planner/select_planner.cpp
            src/execute/select_executor.cpp
            src/execute/insert_executor.cpp
            src/execute/create_table_executor.cpp
            src/execute/drop_table_executor.cpp
            src/execute/describe_executor.cpp
            src/execute/show_tables_executor.cpp
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
                   tests/reentrancy_test.cpp
                   tests/lexer_test.cpp
                   tests/parse_test.cpp
                   tests/expression_test.cpp
                   tests/planner_test.cpp
                   tests/select_exec_test.cpp
                   tests/write_exec_test.cpp
                   tests/create_table_exec_test.cpp
                   tests/show_tables_exec_test.cpp
                   tests/describe_exec_test.cpp)
    target_include_directories(ddbsh-tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests"
                "${CMAKE_CURRENT_SOURCE_DIR}/src")
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

# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# tests/golden/CMakeLists.txt — the golden-parity CTest project for the NEW
# engine (Step 2b, W7 / D23 / D24).
#
# This is a configure-only CMake project (no compilation). It reuses the
# original golden family files (oracle/tests/*.txt) and golden results
# (oracle/tests/results/*.exp) VERBATIM, driving the shipping `dynamodb-shell`
# binary through the external golden harness (golden-runner.sh) instead of the
# original `ddbshtest`. It is intentionally standalone (built separately from
# the engine/CLI/unit-test build via devtools/run-tests) and never links any
# engine code.
#
# How the verbatim reuse works: the original `add_test(... COMMAND ddbshtest ...)`
# lines name `ddbshtest`. Here `ddbshtest` is an IMPORTED executable whose
# location is the configured golden-runner.sh, so every `COMMAND ddbshtest`
# resolves to the harness, which drives `dynamodb-shell`. `${RD}` (the golden
# results dir) is set below exactly as the original tests/ddbsh-tests.txt sets
# it. The original `set_property(TEST ... PASS/FAIL_REGULAR_EXPRESSION ...)`
# lines keep working because CTest applies them to the harness's stdout, which
# the harness emits verbatim for non-`-T` tests.
#
# D24 family routing (2b, interim): only the families whose every case is in the
# 2b command scope (SELECT / EXPLAIN / INSERT / REPLACE / CREATE / DROP) are
# routed here:
#   * ddbsh-symbol-table.txt   (ist-*)   alias generation via EXPLAIN SELECT
#   * ddbsh-select-tests.txt   (isel-*)  the GetItem/Query/Scan decision matrix
#   * ddbsh-insert-tests.txt   (iins-*)  INSERT/REPLACE + SELECT verification
# Every other family (including ddbsh-basic and create-drop/icd-*, both of which
# contain cases outside the 2b scope — UPDATE/DELETE/ratelimit, and
# ALTER/deletion-protection/GSI-throughput respectively) stays on the original
# oracle until its commands port in 2c+. devtools/run-tests excludes the routed
# families from the oracle run so the combined suite stays 583/583.
#
# Configuration inputs (set by devtools/run-tests or on the command line):
#   -DDDBSH_SHELL_BIN=<path>   the shipping dynamodb-shell binary (required)
#   -DDDBSH_SDK_LIB=<path>     AWS SDK shared-library dir (prepended to
#                              LD_LIBRARY_PATH by the harness; optional)
#   -DORACLE_TESTS_DIR=<path>  the original tests/ dir holding the family .txt
#                              files and results/ (defaults to ../../oracle/tests)

cmake_minimum_required(VERSION 3.21)
project(ddbsh-golden NONE)

enable_testing()

set(DDBSH_SHELL_BIN "" CACHE FILEPATH "Path to the shipping dynamodb-shell binary")
set(DDBSH_SDK_LIB "" CACHE PATH "AWS SDK shared-library directory (LD_LIBRARY_PATH)")
set(ORACLE_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../oracle/tests"
    CACHE PATH "Directory holding the original golden family .txt files and results/")

get_filename_component(ORACLE_TESTS_DIR "${ORACLE_TESTS_DIR}" ABSOLUTE)
if(NOT EXISTS "${ORACLE_TESTS_DIR}/ddbsh-select-tests.txt")
    message(FATAL_ERROR
        "ORACLE_TESTS_DIR does not look like the golden tests dir: ${ORACLE_TESTS_DIR}")
endif()

# Configure the harness with the resolved shell/SDK paths baked in as defaults
# (still overridable via the DDBSH_SHELL_BIN / DDBSH_SDK_LIB environment).
set(GOLDEN_RUNNER "${CMAKE_CURRENT_BINARY_DIR}/golden-runner.sh")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/golden-runner.sh.in"
               "${GOLDEN_RUNNER}"
               @ONLY
               FILE_PERMISSIONS
                   OWNER_READ OWNER_WRITE OWNER_EXECUTE
                   GROUP_READ GROUP_EXECUTE
                   WORLD_READ WORLD_EXECUTE)

# `ddbshtest` resolves to the harness, so the original add_test lines run the
# new engine unchanged.
add_executable(ddbshtest IMPORTED GLOBAL)
set_target_properties(ddbshtest PROPERTIES IMPORTED_LOCATION "${GOLDEN_RUNNER}")

# `${RD}` is the golden results dir, exactly as the original ddbsh-tests.txt.
set(RD "${ORACLE_TESTS_DIR}/results")

# Route only the cleanly-in-scope 2b families (D24).
include("${ORACLE_TESTS_DIR}/ddbsh-symbol-table.txt")
include("${ORACLE_TESTS_DIR}/ddbsh-select-tests.txt")
include("${ORACLE_TESTS_DIR}/ddbsh-insert-tests.txt")

# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# tests/golden/CMakeLists.txt — the golden-parity CTest project for the NEW
# engine (Step 2b, W7 / D23 / D24).
#
# This is a configure-only CMake project (no compilation). It reuses the
# original golden family files (oracle/tests/*.txt) and golden results
# (oracle/tests/results/*.exp) VERBATIM, driving the shipping `ddbsh`
# binary through the external golden harness (golden-runner.sh) instead of the
# original `ddbshtest`. It is intentionally standalone (built separately from
# the engine/CLI/unit-test build via devtools/run-tests) and never links any
# engine code.
#
# How the verbatim reuse works: the original `add_test(... COMMAND ddbshtest ...)`
# lines name `ddbshtest`. Here `ddbshtest` is an IMPORTED executable whose
# location is the configured golden-runner.sh, so every `COMMAND ddbshtest`
# resolves to the harness, which drives `ddbsh`. `${RD}` (the golden
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
# Full-engine exposure mode (AI-2a-A): set -DDDBSH_GOLDEN_ALL_FAMILIES=ON to
# register EVERY non-global-tables family here so the whole suite runs against
# the new engine. devtools/run-tests --full-engine drives this to measure the
# implemented-surface parity against the original each phase exit (standing
# directive D-2a-3, 2026-10-15). Unimplemented commands fail as expected until
# their families port; the mode is a measurement, not a gate.
#
# Configuration inputs (set by devtools/run-tests or on the command line):
#   -DDDBSH_SHELL_BIN=<path>   the shipping ddbsh binary (required)
#   -DDDBSH_SDK_LIB=<path>     AWS SDK shared-library dir (prepended to
#                              LD_LIBRARY_PATH by the harness; optional)
#   -DORACLE_TESTS_DIR=<path>  the original tests/ dir holding the family .txt
#                              files and results/ (defaults to ../../oracle/tests)

cmake_minimum_required(VERSION 3.21)
project(ddbsh-golden NONE)

enable_testing()

set(DDBSH_SHELL_BIN "" CACHE FILEPATH "Path to the shipping ddbsh binary")
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

# Full-engine exposure mode (AI-2a-A, D-2a-3 standing directive 2026-10-15):
# when ON, register EVERY non-global-tables golden family so the whole suite
# runs against the new engine, measuring implemented-surface parity against the
# original (devtools/run-tests --full-engine). When OFF (the default), only the
# cleanly-in-scope routed families run on the new engine and devtools/run-tests
# keeps the complement on the oracle, so the combined suite stays whole.
#
# The global-tables families (ddbsh-gt-*) are excluded in both modes: they need
# a multi-region live setup (arch §11) and are commented out in the oracle
# master include (oracle/tests/ddbsh-tests.txt) as well.
option(DDBSH_GOLDEN_ALL_FAMILIES
    "Register every non-global-tables golden family (full-engine exposure mode) instead of only the routed families."
    OFF)

if(DDBSH_GOLDEN_ALL_FAMILIES)
    # Several families reference ${CMAKE_SOURCE_DIR}/tests/*.sql for their `-f`
    # load scripts (e.g. ddbsh-update-tests.txt, ddbsh-create-drop-tests.txt).
    # The oracle master include resolves those relative to the oracle project
    # root; here CMAKE_SOURCE_DIR is tests/golden, so shadow it with the oracle
    # root (the parent of ORACLE_TESTS_DIR) for the duration of the includes so
    # the load-script paths resolve. The oracle family files are reused verbatim
    # — this is the only non-invasive way to satisfy their path assumption.
    get_filename_component(_oracle_root "${ORACLE_TESTS_DIR}" DIRECTORY)
    set(CMAKE_SOURCE_DIR "${_oracle_root}")

    # Mirror the oracle master include order (oracle/tests/ddbsh-tests.txt),
    # minus the commented-out global-tables families.
    include("${ORACLE_TESTS_DIR}/ddbsh-basic-tests.txt")
    include("${ORACLE_TESTS_DIR}/ddbsh-create-drop-tests.txt")
    include("${ORACLE_TESTS_DIR}/ddbsh-pitr-tests.txt")
    include("${ORACLE_TESTS_DIR}/ddbsh-symbol-table.txt")
    include("${ORACLE_TESTS_DIR}/ddbsh-select-tests.txt")
    include("${ORACLE_TESTS_DIR}/ddbsh-update-tests.txt")
    include("${ORACLE_TESTS_DIR}/ddbsh-delete-tests.txt")
    include("${ORACLE_TESTS_DIR}/ddbsh-insert-tests.txt")
    include("${ORACLE_TESTS_DIR}/ddbsh-transactions-tests.txt")
    include("${ORACLE_TESTS_DIR}/ddbsh-return-values.txt")
    include("${ORACLE_TESTS_DIR}/test-cleanup.txt")
    include("${ORACLE_TESTS_DIR}/ddbsh-cross-account.txt")
else()
    # Route the 2b families plus the 2c families (UPDATE/DELETE) to the new
    # engine (D24 + 2c). The update/delete family files reference
    # ${CMAKE_SOURCE_DIR}/tests/*.sql for their `-f` seed loads (resolved
    # against the oracle root in the oracle master include); shadow
    # CMAKE_SOURCE_DIR with the oracle root so those paths resolve here too
    # (the 2b families use no such load, so this is harmless for them).
    get_filename_component(_oracle_root "${ORACLE_TESTS_DIR}" DIRECTORY)
    set(CMAKE_SOURCE_DIR "${_oracle_root}")
    include("${ORACLE_TESTS_DIR}/ddbsh-symbol-table.txt")
    include("${ORACLE_TESTS_DIR}/ddbsh-select-tests.txt")
    include("${ORACLE_TESTS_DIR}/ddbsh-insert-tests.txt")
    include("${ORACLE_TESTS_DIR}/ddbsh-update-tests.txt")
    include("${ORACLE_TESTS_DIR}/ddbsh-delete-tests.txt")
    include("${ORACLE_TESTS_DIR}/ddbsh-transactions-tests.txt")
endif()

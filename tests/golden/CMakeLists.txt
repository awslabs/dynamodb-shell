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
# Family routing history (D24, 2b interim → 2h exit gate): families were routed
# onto the new engine phase by phase (2b: ist-/isel-/iins-; 2c: iut-/delete;
# 2d: itxt-; 2e: icd-/ipt-; 2g: rv-/basic-/cross-account-). As of 2h-1 the route
# is complete: ALL non-global-tables families register here and run on the new
# engine. The global-tables families (ddbsh-gt-*) remain excluded in every mode
# (they need a multi-region live setup, arch §11, and are commented out in the
# oracle master include oracle/tests/ddbsh-tests.txt as well).
#
# Step-2 exit gate (2h-1, D-2h-1, 2026-10-19): the oracle route is now EMPTY —
# every non-global-tables golden family runs against the new engine. The
# per-family DDBSH_GOLDEN_ALL_FAMILIES split and the ROUTED_RE complement on the
# oracle (the 2b two-sub-run scaffold) are retired: there is one build (the new
# engine), one registration (all non-GT families), and the gate exit propagates.
# The oracle/ tree stays as the golden/.exp/ddbsh-wait source (the family .txt
# files and results/*.exp are reused verbatim), but the oracle EXECUTABLE is no
# longer built or run by devtools/run-tests.
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

# Single registration (2h-1): every non-global-tables family runs on the new
# engine. Several families reference ${CMAKE_SOURCE_DIR}/tests/*.sql for their
# `-f` load scripts (ddbsh-update-tests.txt, ddbsh-create-drop-tests.txt, …) and
# ${CMAKE_SOURCE_DIR}/tests/ddbsh-wait for their table-settled / pitr-settled
# poll steps. The oracle master include (oracle/tests/ddbsh-tests.txt) resolves
# those relative to the oracle project root; here CMAKE_SOURCE_DIR is
# tests/golden, so shadow it with the oracle root (the parent of
# ORACLE_TESTS_DIR) for the duration of the includes so the load-script and
# wait-script paths resolve. The oracle family files are reused verbatim — this
# is the only non-invasive way to satisfy their path assumption. The ddbsh-wait
# steps invoke a script (not ddbsh), so they run unchanged under routing.
#
# The include order mirrors the oracle master include exactly, which places
# test-cleanup LAST but one (before cross-account). That ordering matters:
# ctest runs these serially in registration order, and test-cleanup drops the
# tables the data families create, so it must come after them (watch-item,
# D-2h-1 §7). cross-account uses its own account's tables and is unaffected.
get_filename_component(_oracle_root "${ORACLE_TESTS_DIR}" DIRECTORY)
set(CMAKE_SOURCE_DIR "${_oracle_root}")
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

# --- Reimplementation-owned golden families (phase 2i, AI-2i-B) -------------
# New golden families covering command forms the original ddbsh never golden-
# tested. The expected outputs were RECORDED FROM THE ORIGINAL ddbsh (the
# fidelity oracle) via `ddbshtest -R` (D-2i-2), and live in a reimpl-owned tree
# (tests/golden/reimpl/) so the pristine oracle/tests tree stays verbatim.
# ${RD_REIMPL} is the reimpl golden results dir, mirroring how the oracle
# families use ${RD}.
set(RD_REIMPL "${CMAKE_CURRENT_SOURCE_DIR}/reimpl/results")
include("${CMAKE_CURRENT_SOURCE_DIR}/reimpl/ddbsh-help-tests.txt")

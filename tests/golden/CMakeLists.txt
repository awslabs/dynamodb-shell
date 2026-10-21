# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# tests/golden/CMakeLists.txt — the golden-parity CTest project.
#
# This is a configure-only CMake project (no compilation). It drives the
# shipping `ddbsh` binary through the external golden harness (golden-runner.sh)
# and diffs its output against the golden `.exp` files. It is intentionally
# standalone (built separately from the engine/CLI/unit-test build via
# devtools/run-tests) and never links any engine code.
#
# Two golden trees live here:
#   suite/   — the original ddbsh golden suite (family *.txt, results/*.exp,
#              .sql seeds, ddbsh-wait), relocated in-tree during the phase-2i
#              oracle retirement (D-2i-6: the golden contract is kept, never
#              deleted; the old C++11 engine it came with is removed and
#              preserved only in git history under the oracle-archive-v0.8.1
#              tag). These families are the fidelity regression contract.
#   reimpl/  — golden families authored during the reimplementation (phase 2i)
#              for command forms the original never golden-tested; their
#              expected outputs were recorded from the original ddbsh
#              (production `ddbsh-old` binary) before the engine was retired.
#
# How the reuse works: the original `add_test(... COMMAND ddbshtest ...)` lines
# name `ddbshtest`. Here `ddbshtest` is an IMPORTED executable whose location is
# the configured golden-runner.sh, so every `COMMAND ddbshtest` resolves to the
# harness, which drives the shipping `ddbsh`. The family files reference the
# results dir via ${RD} / ${RD_REIMPL} and their seed/.sql + ddbsh-wait inputs
# via ${GOLDEN_SUITE}; all three are set below.
#
# The global-tables families (ddbsh-gt-*) remain excluded in every mode (they
# need a multi-region live setup, arch §11) and are the sole deferred family.
#
# Configuration inputs (set by devtools/run-tests or on the command line):
#   -DDDBSH_SHELL_BIN=<path>    the shipping ddbsh binary (required)
#   -DDDBSH_SDK_LIB=<path>      AWS SDK shared-library dir (optional)
#   -DGOLDEN_SUITE_DIR=<path>   the relocated original golden suite dir
#                               (defaults to ./suite)

cmake_minimum_required(VERSION 3.21)
project(ddbsh-golden NONE)

enable_testing()

set(DDBSH_SHELL_BIN "" CACHE FILEPATH "Path to the shipping ddbsh binary")
set(DDBSH_SDK_LIB "" CACHE PATH "AWS SDK shared-library directory (LD_LIBRARY_PATH)")
set(GOLDEN_SUITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/suite"
    CACHE PATH "Directory holding the original golden family .txt files, results/, .sql seeds, and ddbsh-wait")

get_filename_component(GOLDEN_SUITE_DIR "${GOLDEN_SUITE_DIR}" ABSOLUTE)
if(NOT EXISTS "${GOLDEN_SUITE_DIR}/ddbsh-select-tests.txt")
    message(FATAL_ERROR
        "GOLDEN_SUITE_DIR does not look like the golden suite dir: ${GOLDEN_SUITE_DIR}")
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
# shipping engine unchanged.
add_executable(ddbshtest IMPORTED GLOBAL)
set_target_properties(ddbshtest PROPERTIES IMPORTED_LOCATION "${GOLDEN_RUNNER}")

# Variables the suite family files reference:
#   ${RD}           — the suite's golden results dir (.exp files)
#   ${GOLDEN_SUITE} — the suite dir itself (.sql seed scripts + the ddbsh-wait
#                     poll helper, used by the -f load and table/pitr-settled
#                     wait steps)
set(RD "${GOLDEN_SUITE_DIR}/results")
set(GOLDEN_SUITE "${GOLDEN_SUITE_DIR}")

# --- Original golden suite (fidelity regression contract) -------------------
# The include order places test-cleanup LAST but one (before cross-account):
# ctest runs these serially in registration order, and test-cleanup drops the
# tables the data families create, so it must come after them (D-2h-1 §7).
# cross-account uses its own account's tables and is unaffected.
include("${GOLDEN_SUITE_DIR}/ddbsh-basic-tests.txt")
include("${GOLDEN_SUITE_DIR}/ddbsh-create-drop-tests.txt")
include("${GOLDEN_SUITE_DIR}/ddbsh-pitr-tests.txt")
include("${GOLDEN_SUITE_DIR}/ddbsh-symbol-table.txt")
include("${GOLDEN_SUITE_DIR}/ddbsh-select-tests.txt")
include("${GOLDEN_SUITE_DIR}/ddbsh-update-tests.txt")
include("${GOLDEN_SUITE_DIR}/ddbsh-delete-tests.txt")
include("${GOLDEN_SUITE_DIR}/ddbsh-insert-tests.txt")
include("${GOLDEN_SUITE_DIR}/ddbsh-transactions-tests.txt")
include("${GOLDEN_SUITE_DIR}/ddbsh-return-values.txt")
include("${GOLDEN_SUITE_DIR}/test-cleanup.txt")
include("${GOLDEN_SUITE_DIR}/ddbsh-cross-account.txt")

# --- Reimplementation-owned golden families (phase 2i, AI-2i-B) -------------
# New golden families covering command forms the original ddbsh never golden-
# tested. The expected outputs were RECORDED FROM THE ORIGINAL ddbsh (the
# production `ddbsh-old` binary, D-2i-2) before the engine was retired, and are
# reproduced byte-for-byte by the reimplementation. ${RD_REIMPL} is the reimpl
# golden results dir, mirroring how the suite families use ${RD}.
set(RD_REIMPL "${CMAKE_CURRENT_SOURCE_DIR}/reimpl/results")
include("${CMAKE_CURRENT_SOURCE_DIR}/reimpl/ddbsh-help-tests.txt")
include("${CMAKE_CURRENT_SOURCE_DIR}/reimpl/ddbsh-explain-ddl-tests.txt")
include("${CMAKE_CURRENT_SOURCE_DIR}/reimpl/ddbsh-explain-write-tests.txt")
include("${CMAKE_CURRENT_SOURCE_DIR}/reimpl/ddbsh-show-version-tests.txt")
include("${CMAKE_CURRENT_SOURCE_DIR}/reimpl/ddbsh-show-misc-tests.txt")

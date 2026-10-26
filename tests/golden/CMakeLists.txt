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
# Single golden tree (phase 2j, AI-2j-5 — the provenance-based suite/ + reimpl/
# split was collapsed into one command-family layout; provenance now lives only
# in git history and the discussion record):
#
#   families/   — one manifest .txt per command family (basic, create-drop,
#                 select, insert-replace, update, delete, transactions, pitr,
#                 symbol-table, return-values, cross-account, explain, show,
#                 help, global-tables, test-cleanup). The original add_test
#                 lines are reused verbatim; `ddbshtest` is an IMPORTED target
#                 that resolves to the harness (see below).
#   results/    — ALL golden `.exp` files (the relocated original suite goldens
#                 merged with the reimplementation-authored goldens; byte-
#                 identical to their pre-consolidation content). Referenced as
#                 ${RD}.
#   data/       — the `.sql` seed scripts loaded by the data families via `-f`.
#                 Referenced as ${DATA}.
#   ddbsh-wait  — the region-aware poll-until-state helper used by the control-
#                 plane settling steps (create-drop throughput, pitr, global-
#                 tables). Referenced as ${GOLDEN_DIR}/ddbsh-wait.
#
# The golden `.exp` files, `.sql` seeds, and ddbsh-wait are the fidelity
# regression contract (D-2i-6: the golden contract is kept, never deleted; the
# old C++11 engine it came with is removed and preserved only in git history
# under the oracle-archive-v0.8.1 tag). The reimpl-authored families recorded
# their expected outputs from the original ddbsh (production `ddbsh-old` binary,
# D-2i-2) before the engine was retired.
#
# How the reuse works: the original `add_test(... COMMAND ddbshtest ...)` lines
# name `ddbshtest`. Here `ddbshtest` is an IMPORTED executable whose location is
# the configured golden-runner.sh, so every `COMMAND ddbshtest` resolves to the
# harness, which drives the shipping `ddbsh`.
#
# The global-tables family runs against a real multi-region setup (primary
# us-east-2, replica us-west-1) as of phase 2j (AI-2j-2); it is registered last
# and gated by region-aware ddbsh-wait settling polls.
#
# Configuration inputs (set by devtools/run-tests or on the command line):
#   -DDDBSH_SHELL_BIN=<path>   the shipping ddbsh binary (required)
#   -DDDBSH_SDK_LIB=<path>     AWS SDK shared-library dir (optional)
#   -DGOLDEN_DIR=<path>        the golden tree root (defaults to this dir)

cmake_minimum_required(VERSION 3.21)
project(ddbsh-golden NONE)

enable_testing()

set(DDBSH_SHELL_BIN "" CACHE FILEPATH "Path to the shipping ddbsh binary")
set(DDBSH_SDK_LIB "" CACHE PATH "AWS SDK shared-library directory (LD_LIBRARY_PATH)")
set(GOLDEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    CACHE PATH "Golden tree root (families/, results/, data/, ddbsh-wait)")

get_filename_component(GOLDEN_DIR "${GOLDEN_DIR}" ABSOLUTE)
if(NOT EXISTS "${GOLDEN_DIR}/families/select.txt")
    message(FATAL_ERROR
        "GOLDEN_DIR does not look like the golden tree root: ${GOLDEN_DIR}")
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

# Variables the family files reference:
#   ${RD}         — the golden results dir (.exp files)
#   ${DATA}       — the .sql seed scripts (loaded via `ddbshtest -f`)
#   ${GOLDEN_DIR} — the tree root (holds the ddbsh-wait poll helper)
set(RD "${GOLDEN_DIR}/results")
set(DATA "${GOLDEN_DIR}/data")

# Registration order (ctest runs serially in registration order):
#   * The data families come first; they create the tables the next ones query.
#   * test-cleanup drops the data-family tables, so it comes AFTER them
#     (D-2h-1 §7).
#   * cross-account uses its own account's tables and is unaffected by cleanup.
#   * explain / show / help are self-contained (each creates+drops its own
#     tables or needs none), so their position is independent.
#   * global-tables is self-contained (its own `gttest` table in
#     us-east-2/us-west-1) and is registered LAST, contiguous
#     (setup -> check -> completion), gated by region-aware ddbsh-wait polls.
set(GOLDEN_FAMILIES
    basic
    create-drop
    pitr
    symbol-table
    select
    update
    delete
    insert-replace
    composite-gsi
    vector
    transactions
    return-values
    test-cleanup
    cross-account
    help
    explain
    show
    global-tables)

foreach(family IN LISTS GOLDEN_FAMILIES)
    include("${GOLDEN_DIR}/families/${family}.txt")
endforeach()

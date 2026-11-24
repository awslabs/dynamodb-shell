# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# Top-level CMake for the ddbsh reimplementation (D14).
#
# Targets from one build:
#   ddbsh          — the engine library (SDK-free public API; no terminal)
#   ddbsh-aws      — OPTIONAL AWS-backed client component (alias ddbsh::aws;
#                    file libddbsh-aws). Depends on the engine PLUS the AWS SDK;
#                    provides the ready-made ddbsh::aws::AwsDDBClient /
#                    AwsApiGuard shared by BOTH the CLI and external embedders.
#                    Built when DDBSH_BUILD_AWS is ON (default). OFF yields a
#                    pure SDK-free engine-only build/install (and skips the CLI,
#                    which needs the client).
#   ddbsh-cli      — the thin CLI (readline + ddbsh::aws); the executable file
#                    is named `ddbsh` (OUTPUT_NAME), distinct from the `ddbsh`
#                    library target (file: libddbsh). Requires DDBSH_BUILD_AWS.
#   ddbsh-tests    — native GoogleTest unit tests + the boundary check
#
# The build also installs a find_package(ddbsh) CMake package exporting
# ddbsh::ddbsh and (when DDBSH_BUILD_AWS) ddbsh::aws, so an external consumer
# links the engine and the AWS client with no hand-written include/lib wiring.
#
# The compiler is NOT hardcoded (choose via CMakePresets or -DCMAKE_CXX_COMPILER).
# C++23 is required for std::expected (D12).
#
# NOTE (2a reorg, P2a-01 D-Q3; 2i oracle retirement): the reimplementation owns
# the whole tree (include/ src/ cli/ tests/). The original C++11 ddbsh engine —
# the fidelity oracle and golden-regeneration source — was removed from the
# working tree during phase 2i and is preserved only in git history under the
# oracle-archive-v0.8.1 tag. Its golden contract was relocated to
# tests/golden and remains the regression contract, exercised by
# devtools/run-tests (the standalone tests/golden CMake project) against real
# DynamoDB.

cmake_minimum_required(VERSION 3.21)

project(ddbsh
        VERSION 0.12.4
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
# The optional AWS-backed client component (alias ddbsh::aws) + the CLI, which
# needs it. ON by default. OFF yields a pure SDK-free engine-only build/install
# that needs no AWS SDK at all (minimal/portability scenarios, the offline
# embed_demo path); the CLI is skipped because it requires the client.
option(DDBSH_BUILD_AWS "Build the optional AWS-backed client component and the CLI" ON)

# The Emscripten/WASM web target (P6d). OFF by default so the normal Linux CI
# matrix needs no Emscripten; turned ON only by the `wasm-release` preset (which
# also sets DDBSH_BUILD_AWS=OFF and DDBSH_BUILD_TESTS=OFF). When ON, it requires
# the Emscripten toolchain (configure via `emcmake cmake --preset wasm-release`);
# web/CMakeLists.txt FATAL_ERRORs on a non-Emscripten configure so a
# misinvocation fails fast. See web/README.md.
option(DDBSH_BUILD_WEB "Build the Emscripten/WASM web target" OFF)

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

# --- Coverage instrumentation (phase 2i, AI-2i-C) ---------------------------
# Opt-in line/branch coverage for a dedicated measurement build. OFF by default
# so the normal build and the -Werror gate are unaffected. When ON, every one
# of OUR targets (engine library, cli-core, cli, tests) is instrumented via the
# ddbsh_warnings interface they all link; third-party code (AWS SDK, GoogleTest)
# is untouched. Use a separate build directory (e.g. build-coverage) and report
# with gcovr/lcov. Not supported with MSVC (use a GCC/Clang build for coverage).
#   cmake -S . -B build-coverage -DDDBSH_COVERAGE=ON -DDDBSH_WARNINGS_AS_ERRORS=OFF ...
#   cmake --build build-coverage
#   (run the suite) ; gcovr --gcov-executable gcc14-gcov ...
option(DDBSH_COVERAGE "Build with gcov/lcov coverage instrumentation" OFF)
if(DDBSH_COVERAGE)
    if(MSVC)
        message(FATAL_ERROR "DDBSH_COVERAGE requires a GCC/Clang toolchain (not MSVC)")
    endif()
    # -O0 keeps line mapping faithful; --coverage sets both compile and link.
    target_compile_options(ddbsh_warnings INTERFACE --coverage -O0 -g)
    target_link_options(ddbsh_warnings INTERFACE --coverage)
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
            src/retry_event.cpp
            src/connection/connection.cpp
            src/render/renderer.cpp
            src/render/format.cpp
            src/parser/driver.cpp
            src/parser/param_bind.cpp
            src/parser/lex_debug.cpp
            src/parser/help_text.cpp
            src/expressions/symbol_table.cpp
            src/expressions/attribute_serialize.cpp
            src/planner/get_key_schema.cpp
            src/planner/select_planner.cpp
            src/execute/select_executor.cpp
            src/execute/select_cursor.cpp
            src/execute/insert_executor.cpp
            src/execute/update_executor.cpp
            src/execute/delete_executor.cpp
            src/execute/rate_limiter.cpp
            src/execute/create_table_executor.cpp
            src/execute/alter_table_executor.cpp
            src/execute/backup_restore_executor.cpp
            src/execute/show_commands_executor.cpp
            src/execute/drop_table_executor.cpp
            src/execute/describe_executor.cpp
            src/execute/show_tables_executor.cpp
            src/execute/transact_build.cpp
            src/execute/transact_executor.cpp
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

# PUBLIC include uses BUILD_INTERFACE / INSTALL_INTERFACE so the target is
# installable + exportable (find_package(ddbsh), D6): build consumers see the
# source include/, installed consumers see <prefix>/include.
target_include_directories(ddbsh
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${CMAKE_CURRENT_BINARY_DIR}/generated"
            "${GEN_DIR}")
# $<BUILD_INTERFACE:> so the warnings interface applies during the build but is
# not recorded in the installed/exported link interface (a static engine build
# would otherwise propagate it as $<LINK_ONLY:...> and break install(EXPORT),
# since ddbsh_warnings is not itself exported). Harmless for the default shared
# build, where a PRIVATE dep does not propagate anyway.
target_link_libraries(ddbsh PRIVATE $<BUILD_INTERFACE:ddbsh_warnings>)
set_target_properties(ddbsh PROPERTIES OUTPUT_NAME ddbsh)

# The generated flex/bison sources are not ours to lint; suppress -Werror on
# just those translation units.
set_source_files_properties(
    "${GEN_DIR}/grammar.cpp" "${GEN_DIR}/lexer.cpp"
    PROPERTIES COMPILE_OPTIONS "-Wno-error")

# --- ddbsh-wire: SDK-free AWS JSON 1.0 codec (ddbsh::wire, P6 / D-P6-13) ------
# The JSON-string transport counterpart to ddbsh-aws (the AWS SDK transport):
# serialize(request) -> (operation, AWS-JSON-1.0 body), and — in a later P6
# sub-phase — parse(response) over the engine's SDK-free request/response
# structs. It is portable, SDK-free, and terminal-free, built on ALL platforms
# so its host unit tests run in the normal CI matrix independent of the
# Emscripten target. The browser (Emscripten, DDBSH_BUILD_AWS=OFF) build links
# this and NOT ddbsh-aws. It PUBLIC-links the engine purely for its SDK-free
# public headers (the request structs); it uses no engine runtime symbols, and
# it never pulls the AWS SDK (an engine_boundary_check-style invariant: nothing
# under include/ddbsh/wire/ includes <aws/...>).
add_library(ddbsh-wire wire/codec.cpp wire/parse.cpp wire/json_reader.cpp wire/datetime.cpp wire/wire_client.cpp)
add_library(ddbsh::wire ALIAS ddbsh-wire)
# EXPORT_NAME `wire` so the INSTALLED imported target is `ddbsh::wire`, matching
# the in-tree alias (the NAMESPACE `ddbsh::` prefixes the EXPORT_NAME).
set_target_properties(ddbsh-wire PROPERTIES OUTPUT_NAME ddbsh-wire EXPORT_NAME wire
                      POSITION_INDEPENDENT_CODE ON)
target_include_directories(ddbsh-wire
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    # The repo root carries the component's INTERNAL headers (wire/json_reader.hpp,
    # the SDK-free JSON reader the parser uses). It is PRIVATE + BUILD_INTERFACE
    # only — not part of the installed/exported surface, exactly like the engine's
    # private src/ include — so json_reader.hpp never reaches a consumer.
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
# ddbsh_warnings wrapped in $<BUILD_INTERFACE:> so it is not recorded in the
# installed/exported link interface (it is not itself an exported target).
target_link_libraries(ddbsh-wire
    PUBLIC  ddbsh::ddbsh
    PRIVATE $<BUILD_INTERFACE:ddbsh_warnings>)

# --- CLI driver core (terminal-free, engine-only) ----------------------------
# The terminal-free CLI driver (isql batch assembly + execute/exit loop) lives
# in its own static lib so it can be linked by BOTH the CLI binary and the
# native unit tests (the tests drive it offline against a FakeDDBClient-backed
# Session, closing the carried isql-assembly test gap, 2h-3). It depends only on
# the engine library — no readline, no AWS SDK — so the library/CLI boundary and
# the no-terminal-dependency invariant hold (tenet 3 / D6). It is built
# unconditionally (the unit tests link it even when DDBSH_BUILD_AWS is OFF).
add_library(ddbsh-cli-core STATIC cli/repl.cpp)
target_include_directories(ddbsh-cli-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/cli")
target_link_libraries(ddbsh-cli-core PUBLIC ddbsh::ddbsh PRIVATE ddbsh_warnings)

# --- Optional AWS-backed client component (ddbsh::aws) + the CLI --------------
# ddbsh-aws is the single, shared AWS translation used by BOTH the CLI and
# external embedders (D1/D2). It sits on top of the SDK-free engine and pulls
# the AWS SDK; the CLI links it rather than carrying its own copy of the
# adapter. Built only when DDBSH_BUILD_AWS is ON (the CLI needs the client, so
# OFF also skips the CLI — an engine-only build/install needs no AWS SDK).
if(DDBSH_BUILD_AWS)
    find_package(AWSSDK REQUIRED COMPONENTS dynamodb sts iam)
    find_package(ZLIB REQUIRED)

    # The AWS-backed IDDBClient adapter + the SDK-lifecycle guard, as a library
    # target (alias ddbsh::aws, file libddbsh-aws). PUBLIC-links the engine and
    # the AWS SDK because the public header include/ddbsh/aws/aws_ddb_client.hpp
    # inherits ddbsh::IDDBClient and includes <aws/dynamodb/DynamoDBClient.h> —
    # a consumer compiling against it needs both on the usage interface (D1).
    #
    # STATIC (not BUILD_SHARED_LIBS-following, a deliberate implementation
    # decision): the AWS SDK for C++ is distributed/built as STATIC archives in
    # this toolchain. A SHARED libddbsh-aws would embed the static SDK, while an
    # embedder's executable — which constructs the header-inline AwsApiGuard
    # (Aws::InitAPI) — transitively pulls its OWN copy of the static SDK. Two
    # copies of the SDK's global state (memory system, default allocators) then
    # exist across the .so/exe boundary: InitAPI initializes one, the
    # DynamoDBClient uses the other, and the process segfaults. A STATIC
    # ddbsh-aws links the SDK into the final binary exactly once regardless of
    # where AwsApiGuard is instantiated, which is correct for both a static and
    # a shared SDK. (TODO(portability): revisit if a shared AWS SDK build is
    # adopted and a shared component is wanted.)
    add_library(ddbsh-aws STATIC aws/aws_ddb_client.cpp aws/database.cpp
                                 aws/retry_observer_strategy.cpp)
    add_library(ddbsh::aws ALIAS ddbsh-aws)
    # EXPORT_NAME `aws` so the INSTALLED imported target is `ddbsh::aws` (the
    # NAMESPACE `ddbsh::` prefixes the EXPORT_NAME, not the raw target name) —
    # matching the in-tree alias so consumers link the same name either way.
    set_target_properties(ddbsh-aws PROPERTIES OUTPUT_NAME ddbsh-aws EXPORT_NAME aws
                          POSITION_INDEPENDENT_CODE ON)
    target_include_directories(ddbsh-aws
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:include>)
    # ddbsh_warnings is wrapped in $<BUILD_INTERFACE:> so it applies during the
    # build but is NOT recorded in the installed/exported link interface — a
    # STATIC lib would otherwise propagate the private dep as $<LINK_ONLY:...>
    # and install(EXPORT) would fail because ddbsh_warnings is not exported.
    target_link_libraries(ddbsh-aws
        PUBLIC  ddbsh::ddbsh ${AWSSDK_LINK_LIBRARIES}
        PRIVATE $<BUILD_INTERFACE:ddbsh_warnings>)

    # Target name `ddbsh-cli` is distinct from the `ddbsh` library target; the
    # OUTPUT_NAME makes the installed executable `ddbsh` (the shipping binary
    # name, human decision 2026-10-14). No file collision: the library file is
    # `libddbsh.{so,a}`, the executable is `ddbsh`. The CLI links ddbsh::aws and
    # inherits the AWS SDK transitively (PUBLIC), so it no longer names the SDK
    # link libraries or the adapter source directly.
    add_executable(ddbsh-cli cli/main.cpp)
    set_target_properties(ddbsh-cli PROPERTIES OUTPUT_NAME ddbsh)
    target_include_directories(ddbsh-cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/cli")
    target_link_libraries(ddbsh-cli
        PRIVATE ddbsh::ddbsh ddbsh::aws ddbsh-cli-core ddbsh_warnings readline)
endif()

# --- Emscripten/WASM web target (P6d) ----------------------------------------
# The browser build: engine + wire + cli-core linked into a WASM module with an
# embind surface and the Asyncify transport. Opt-in (DDBSH_BUILD_WEB, default
# OFF) and Emscripten-only. It needs the SDK-free libraries only — never
# ddbsh-aws, never the CLI — so it coexists with DDBSH_BUILD_AWS=OFF (the
# wasm-release preset sets exactly that). See web/CMakeLists.txt.
if(DDBSH_BUILD_WEB)
    add_subdirectory(web)
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
                   tests/connection_api_test.cpp
                   tests/prepared_test.cpp
                   tests/memory_client_test.cpp
                   tests/reentrancy_test.cpp
                   tests/lexer_test.cpp
                   tests/parse_test.cpp
                   tests/batch_test.cpp
                   tests/help_test.cpp
                   tests/expression_test.cpp
                   tests/planner_test.cpp
                   tests/select_exec_test.cpp
                   tests/select_stream_test.cpp
                   tests/timely_output_test.cpp
                   tests/format_test.cpp
                   tests/write_exec_test.cpp
                   tests/update_delete_exec_test.cpp
                   tests/return_values_ratelimit_test.cpp
                   tests/transact_exec_test.cpp
                   tests/create_table_exec_test.cpp
                   tests/alter_table_exec_test.cpp
                   tests/backup_restore_exec_test.cpp
                   tests/show_commands_exec_test.cpp
                   tests/show_tables_exec_test.cpp
                   tests/describe_exec_test.cpp
                   tests/explain_exec_test.cpp
                   tests/connect_reconnect_test.cpp
                   tests/cli_exit_code_test.cpp
                   tests/cli_ca_bundle_test.cpp
                   tests/cli_retry_line_test.cpp
                   tests/wire_json_reader_test.cpp
                   tests/wire_parse_roundtrip_test.cpp
                   tests/wire_parse_control_test.cpp
                   tests/wire_datetime_test.cpp
                   tests/wire_client_test.cpp
                   tests/wire_client_control_test.cpp
                   tests/wire_client_retry_test.cpp)
    target_include_directories(ddbsh-tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests"
                "${CMAKE_CURRENT_SOURCE_DIR}/src"
                "${CMAKE_CURRENT_SOURCE_DIR}/cli"
                # Repo root for the wire component's internal header
                # (wire/json_reader.hpp), which wire_json_reader_test drives
                # directly. PRIVATE to the test binary only.
                "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(ddbsh-tests
        PRIVATE ddbsh::ddbsh ddbsh::wire ddbsh-cli-core ddbsh_warnings GTest::gtest_main)

    include(GoogleTest)
    # PRE_TEST discovery enumerates tests at ctest time (not build time), so the
    # binary is not executed during the build. This matters for the sanitizer
    # preset, where the ASan runtime (libasan) is not on the default loader path
    # at build time; ctest runs with the proper environment.
    gtest_discover_tests(ddbsh-tests DISCOVERY_MODE PRE_TEST)

    add_test(NAME engine_boundary_check
             COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/boundary_check.sh"
                     "${CMAKE_CURRENT_SOURCE_DIR}")

    # --- ddbsh-wire EXPLAIN-vs-wire differential gate (P6a, P6-02 §9.1) ------
    # The SDK-free ddbsh::wire serializer becomes a production codec the live
    # browser transport depends on, so every byte it emits must match the AWS
    # SDK. This test asserts serialize(in).body == AwsDDBClient::explain_request
    # (in) (the raw SDK SerializePayload bytes) for a corpus covering every
    # field and AttributeValue type of the data-plane ops. It needs the SDK
    # (ddbsh::aws), so it is built only with DDBSH_BUILD_AWS; it makes no network
    # call (serialization is offline). It provides its own main() to wrap the
    # whole process in one AwsApiGuard, so it links GTest::gtest (not
    # gtest_main). add_dependencies ties it to ddbsh-tests so run-tests'
    # `--target ddbsh-tests` native step builds it and ctest runs it.
    if(DDBSH_BUILD_AWS)
        add_executable(ddbsh-wire-diff-tests tests/wire_serialize_diff_test.cpp)
        target_include_directories(ddbsh-wire-diff-tests
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_link_libraries(ddbsh-wire-diff-tests
            PRIVATE ddbsh::wire ddbsh::aws ddbsh_warnings GTest::gtest)
        add_test(NAME ddbsh_wire_serialize_diff COMMAND ddbsh-wire-diff-tests)
        add_dependencies(ddbsh-tests ddbsh-wire-diff-tests)

        # P8 (D-P8-1/5): the AWS-adapter profile `ca_bundle` reader
        # (ddbsh::aws::profile_ca_bundle) read offline against a temp AWS config
        # file. It needs the SDK (ddbsh::aws) and the SDK config cache, so it is
        # built only with DDBSH_BUILD_AWS. It provides its own main() to point
        # AWS_CONFIG_FILE at the temp file BEFORE InitAPI and wrap the process in
        # one AwsApiGuard, so it links GTest::gtest (not gtest_main). It makes no
        # network call (a pure config-file parse). add_dependencies ties it to
        # ddbsh-tests so run-tests' native step builds it and ctest runs it.
        add_executable(ddbsh-aws-profile-ca-bundle-tests tests/aws_profile_ca_bundle_test.cpp)
        target_include_directories(ddbsh-aws-profile-ca-bundle-tests
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_link_libraries(ddbsh-aws-profile-ca-bundle-tests
            PRIVATE ddbsh::aws ddbsh_warnings GTest::gtest)
        add_test(NAME ddbsh_aws_profile_ca_bundle COMMAND ddbsh-aws-profile-ca-bundle-tests)
        add_dependencies(ddbsh-tests ddbsh-aws-profile-ca-bundle-tests)

        # P12 (D-P12-18, test 6): the AWS SDK retry decorator
        # (aws/retry_observer_strategy.*) against a fake inner strategy, and
        # AwsDDBClient wiring it in rebuild(), driven against a refused
        # loopback port with dummy credentials. Offline: nothing leaves the
        # host. It includes the component's internal header from aws/ (PRIVATE
        # to this test) and has its own main() (one AwsApiGuard), so it links
        # GTest::gtest.
        add_executable(ddbsh-aws-retry-observer-tests tests/aws_retry_observer_test.cpp)
        target_include_directories(ddbsh-aws-retry-observer-tests
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${CMAKE_CURRENT_SOURCE_DIR}/aws")
        target_link_libraries(ddbsh-aws-retry-observer-tests
            PRIVATE ddbsh::aws ddbsh_warnings GTest::gtest)
        add_test(NAME ddbsh_aws_retry_observer COMMAND ddbsh-aws-retry-observer-tests)
        add_dependencies(ddbsh-tests ddbsh-aws-retry-observer-tests)
    endif()

    # D19 (P4d): the embedding API's own misuse messages must never appear in a
    # golden .exp — the CLI does not drive those paths, so the 705/705 CLI
    # fidelity contract stays intact. This gate makes that guarantee mechanical.
    add_test(NAME golden_absence_check
             COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden_absence_check.sh"
                     "${CMAKE_CURRENT_SOURCE_DIR}")

    # P11 F1: the Node that devtools/vendor-aws-sdk-js and devtools/lib-web.sh
    # pick (web_select_node): a Node 20+ on PATH wins over emsdk's bundled
    # Node. Offline: stubs stand in for Node, npm and emcmake.
    add_test(NAME web_node_selection_check
             COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/web_node_selection_check.sh"
                     "${CMAKE_CURRENT_SOURCE_DIR}")

    # OFFLINE embedding sample (D29): built in-tree against the SDK-free engine
    # and run as a default-on, credential-less CTest. It drives the embedding
    # API (ddbsh::Connection) over the in-memory ddbsh::testing::MemoryClient,
    # so it compiles + runs anywhere with no AWS SDK and no credentials,
    # restoring the offline example + CI signal the retired embed_demo provided.
    # (Its own standalone examples/ddbshlib_inmemory_sample/CMakeLists.txt
    # additionally proves it as an EXTERNAL find_package(ddbsh) consumer.)
    add_executable(ddbshlib_inmemory_sample
                   examples/ddbshlib_inmemory_sample/ddbshlib_inmemory_sample.cpp)
    target_link_libraries(ddbshlib_inmemory_sample PRIVATE ddbsh::ddbsh ddbsh_warnings)
    add_test(NAME ddbshlib_inmemory_sample COMMAND ddbshlib_inmemory_sample)
    # Build the offline sample whenever the test binary is built, so a
    # `--target ddbsh-tests` build (the gate's native step) produces it and its
    # default-on CTest runs rather than reporting "Not Run".
    add_dependencies(ddbsh-tests ddbshlib_inmemory_sample)

    # Phase-5 FEATURE sample (D9): a worked, heavily-commented tour of the P5
    # embedding surface — prepared statements + re-exec, typed set/binary bind,
    # lazy streaming (small page size), and transactions — driven offline over
    # the in-memory ddbsh::testing::MemoryClient. Built in-tree against the
    # SDK-free engine and run as a default-on, credential-less CTest, exactly
    # like ddbshlib_inmemory_sample. Its own standalone
    # examples/ddbshlib_features_sample/CMakeLists.txt additionally proves it as
    # an EXTERNAL find_package(ddbsh) consumer.
    add_executable(ddbshlib_features_sample
                   examples/ddbshlib_features_sample/ddbshlib_features_sample.cpp)
    target_link_libraries(ddbshlib_features_sample PRIVATE ddbsh::ddbsh ddbsh_warnings)
    add_test(NAME ddbshlib_features_sample COMMAND ddbshlib_features_sample)
    add_dependencies(ddbsh-tests ddbshlib_features_sample)

    # LIVE embedding sample (D28): built in-tree against ddbsh::aws and run
    # against REAL DynamoDB (us-east-1) via the standard ~/.aws chain. OFF by
    # default because it needs credentials and SPENDS (it creates+drops a real
    # on-demand table). Requires DDBSH_BUILD_AWS. Enable with
    # -DDDBSH_TEST_LIVE_SAMPLE=ON (the gate's live path passes it). Its own
    # standalone examples/ddbshlib_sample/CMakeLists.txt proves it as an
    # EXTERNAL find_package(ddbsh) consumer.
    option(DDBSH_TEST_LIVE_SAMPLE
        "Build + run the live embedding sample against real DynamoDB as a CTest."
        OFF)
    if(DDBSH_TEST_LIVE_SAMPLE AND DDBSH_BUILD_AWS)
        add_executable(ddbshlib_sample examples/ddbshlib_sample/ddbshlib_sample.cpp)
        target_link_libraries(ddbshlib_sample PRIVATE ddbsh::aws ddbsh_warnings)
        add_test(NAME ddbshlib_sample COMMAND ddbshlib_sample)

        # Real-DynamoDB functional tests of the embedding API (acceptance
        # criterion 1). A separate GoogleTest binary linking ddbsh::aws — it
        # SPENDS (creates/drops a real table), so it is gated the same way as
        # the live sample and is never part of the default offline suite.
        add_executable(ddbsh-live-tests tests/connection_live_test.cpp)
        target_include_directories(ddbsh-live-tests
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_link_libraries(ddbsh-live-tests
            PRIVATE ddbsh::aws ddbsh_warnings GTest::gtest_main)
        add_test(NAME ddbsh_live_api COMMAND ddbsh-live-tests)
    endif()
endif()

# --- Install + find_package(ddbsh) package (D6) ------------------------------
# Ship a real CMake package so an external consumer links the engine (and,
# optionally, the AWS client) via find_package(ddbsh) with no hand-written
# include/lib wiring. Two export sets under lib/cmake/ddbsh:
#   ddbsh-targets      — the SDK-free engine (always; defines ddbsh::ddbsh)
#   ddbsh-aws-targets  — the AWS component (only when DDBSH_BUILD_AWS; defines
#                        ddbsh::aws). The package config find_dependency(AWSSDK)
#                        before pulling it in, guarded so an engine-only install
#                        needs no SDK at all.
include(CMakePackageConfigHelpers)

install(TARGETS ddbsh ddbsh-wire
        EXPORT ddbsh-targets
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
install(EXPORT ddbsh-targets
        FILE ddbsh-targets.cmake
        NAMESPACE ddbsh::
        DESTINATION lib/cmake/ddbsh)

if(DDBSH_BUILD_AWS)
    # The SDK-dependent headers (include/ddbsh/aws/) install alongside the
    # SDK-free ones only when the component is built.
    install(DIRECTORY include/ddbsh DESTINATION include)

    install(TARGETS ddbsh-aws
            EXPORT ddbsh-aws-targets
            RUNTIME DESTINATION bin
            LIBRARY DESTINATION lib
            ARCHIVE DESTINATION lib)
    install(EXPORT ddbsh-aws-targets
            FILE ddbsh-aws-targets.cmake
            NAMESPACE ddbsh::
            DESTINATION lib/cmake/ddbsh)
    # The CLI binary installs to bin/ but is NOT an exported CMake target
    # (consumers link the library, not the executable).
    install(TARGETS ddbsh-cli RUNTIME DESTINATION bin)
else()
    # Engine-only install: omit the SDK-dependent aws/ headers.
    install(DIRECTORY include/ddbsh DESTINATION include
            PATTERN "aws" EXCLUDE)
endif()

configure_package_config_file(
    cmake/ddbsh-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/ddbsh-config.cmake"
    INSTALL_DESTINATION lib/cmake/ddbsh)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ddbsh-config-version.cmake"
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ddbsh-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ddbsh-config-version.cmake"
    DESTINATION lib/cmake/ddbsh)

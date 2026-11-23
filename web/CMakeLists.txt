# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# web/CMakeLists.txt — the Emscripten/WASM browser edition (P6d; P9).
#
# Builds ONE WebAssembly module, `ddbsh-wasm` (OUTPUT_NAME ddbsh ->
# ddbsh.js/ddbsh.wasm), that runs the SDK-free engine — lexer, parser,
# planner, executor, renderer, ddbsh-wire codec, WireClient and the
# terminal-free CLI driver (ddbsh-cli-core) — over JsSdkTransport, which hands
# each serialized request to the page's vendored AWS SDK for JavaScript v3
# bridge (web/sdk-bridge.mjs). The SDK signs (SigV4) and sends it straight to
# Amazon DynamoDB; there is no server-side component (P9, D-P9-1).
#
# The module links NO AWS SDK for C++, NO readline, NO CLI binary. It handles
# no credentials itself: the user's short-term credentials stay in the JS
# bridge's closure.
#
# Configured only under the Emscripten toolchain (emcmake). Guarded by
# DDBSH_BUILD_WEB (default OFF) so the normal CI matrix needs no Emscripten.

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "DDBSH_BUILD_WEB requires the Emscripten toolchain. "
        "Configure with `emcmake cmake --preset wasm-release` "
        "(see web/README.md for the pinned emsdk setup).")
endif()

# Emscripten link flags:
#   -sASYNCIFY            suspend/resume the C++ stack across the SDK bridge await
#   --bind               embind (the DdbshSession class + RunResult value_object)
#   -sMODULARIZE/ES6     emit an ES6 module exporting a factory (createDdbsh)
#   -sEXPORT_NAME        the factory name the page and the Node shim import
#   -sALLOW_MEMORY_GROWTH the engine's working set is input-dependent
#   -sDYNAMIC_EXECUTION=0 no eval / new Function in the emitted glue, so the page
#                        runs under `script-src 'self' 'wasm-unsafe-eval'`
#                        (D-P9-02-9, F10); run-web-smoke and CI grep the glue
#   -sEXPORTED_RUNTIME_METHODS  the UTF8 helpers the Asyncify bridge's JS uses
# Pinned to the emsdk documented in web/README.md; revisit flags on an emsdk bump.
set(DDBSH_WEB_LINK_OPTIONS
    "-sASYNCIFY"
    "--bind"
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    "-sEXPORT_NAME=createDdbsh"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sDYNAMIC_EXECUTION=0"
    "-sEXPORTED_RUNTIME_METHODS=['UTF8ToString','stringToUTF8','lengthBytesUTF8']")

add_executable(ddbsh-wasm bindings.cpp js_sdk_transport.cpp)
set_target_properties(ddbsh-wasm PROPERTIES OUTPUT_NAME ddbsh)
# bindings.cpp includes "repl.hpp" (the shared CLI driver) from cli/.
target_include_directories(ddbsh-wasm PRIVATE "${CMAKE_SOURCE_DIR}/cli" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ddbsh-wasm
    PRIVATE ddbsh::ddbsh ddbsh::wire ddbsh-cli-core ddbsh_warnings)
target_link_options(ddbsh-wasm PRIVATE ${DDBSH_WEB_LINK_OPTIONS})

# Stage ddbsh's static bundle beside the emitted module, so the output dir is
# BOTH directly servable (any static server) and the environment the Node
# gates run in:
#   index.html app.js app-core.mjs sdk-bridge.mjs styles.css   the page
#   vendor/aws-sdk/{aws-sdk-dynamodb.mjs, allowed-hosts.mjs,
#                   PROVENANCE.json, THIRD-PARTY-NOTICES.txt}    the pinned SDK
#   ddbsh-node.mjs        the CLI-shaped Node shim the web golden gate drives
#   live-smoke.mjs        the live UI-path smoke devtools/run-web-smoke runs
#   app-core.test.mjs sdk-bridge.test.mjs streaming.test.mjs package.test.mjs
#                         the offline Node tests (streaming.test.mjs runs the
#                         built ddbsh.wasm with stub transport hooks)
#   bundle-files.txt      the publish allowlist (read by package.test.mjs)
#   package.cmake         the packaging script (package.test.mjs runs it on
#                         bad input to check that it refuses each case)
#   package.json          marks the dir as ES modules for Node
# One copy rule per file, each depending on its source, so an edit to any of
# them is re-staged by the next build. (Before P10 this was a POST_BUILD step
# of ddbsh-wasm, which ran only when the module relinked; an edited page or
# test file stayed stale in the build folder.)
set(DDBSH_WEB_STAGED_FILES
    index.html app.js app-core.mjs sdk-bridge.mjs styles.css
    ddbsh-node.mjs live-smoke.mjs
    app-core.test.mjs sdk-bridge.test.mjs streaming.test.mjs package.test.mjs
    bundle-files.txt package.cmake package.json
    vendor/aws-sdk/aws-sdk-dynamodb.mjs vendor/aws-sdk/allowed-hosts.mjs
    vendor/aws-sdk/PROVENANCE.json vendor/aws-sdk/THIRD-PARTY-NOTICES.txt)
set(ddbsh_web_staged_outputs "")
foreach(rel IN LISTS DDBSH_WEB_STAGED_FILES)
    # CMAKE_CURRENT_BINARY_DIR is the module's output dir (web/ddbsh.js).
    set(staged_path "${CMAKE_CURRENT_BINARY_DIR}/${rel}")
    get_filename_component(staged_dir "${staged_path}" DIRECTORY)
    add_custom_command(
        OUTPUT "${staged_path}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${staged_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/${rel}" "${staged_path}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${rel}"
        COMMENT "Staging web/${rel}"
        VERBATIM)
    list(APPEND ddbsh_web_staged_outputs "${staged_path}")
endforeach()
add_custom_target(ddbsh-web-stage ALL DEPENDS ${ddbsh_web_staged_outputs})
add_dependencies(ddbsh-web-stage ddbsh-wasm)

# The packaging step (P10, D-P10-7): copy exactly the files in bundle-files.txt
# into build-preset/wasm-release/dist/ddbsh-web-<version>/, plus a zip of that
# folder and SHA256SUMS (see package.cmake). Not part of ALL; run it with
#   cmake --build build-preset/wasm-release --target ddbsh-web-package
set(DDBSH_WEB_BUNDLE_NAME "ddbsh-web-${PROJECT_VERSION}")
add_custom_target(ddbsh-web-package
    COMMAND ${CMAKE_COMMAND}
            "-DWEB_OUT=${CMAKE_CURRENT_BINARY_DIR}"
            "-DMANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/bundle-files.txt"
            "-DDIST_DIR=${CMAKE_BINARY_DIR}/dist"
            "-DBUNDLE_NAME=${DDBSH_WEB_BUNDLE_NAME}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/package.cmake"
    COMMENT "Packaging ${DDBSH_WEB_BUNDLE_NAME} into ${CMAKE_BINARY_DIR}/dist"
    VERBATIM)
add_dependencies(ddbsh-web-package ddbsh-web-stage)

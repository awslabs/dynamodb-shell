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
#   app-core.test.mjs sdk-bridge.test.mjs   the offline Node unit tests
#   package.json          marks the dir as ES modules for Node
add_custom_command(TARGET ddbsh-wasm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/app.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/app-core.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/sdk-bridge.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/styles.css"
            "${CMAKE_CURRENT_SOURCE_DIR}/ddbsh-node.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/app-core.test.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/sdk-bridge.test.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/package.json"
            "$<TARGET_FILE_DIR:ddbsh-wasm>/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ddbsh-wasm>/vendor/aws-sdk"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/aws-sdk/aws-sdk-dynamodb.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/aws-sdk/allowed-hosts.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/aws-sdk/PROVENANCE.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/aws-sdk/THIRD-PARTY-NOTICES.txt"
            "$<TARGET_FILE_DIR:ddbsh-wasm>/vendor/aws-sdk/"
    COMMENT "Staging the ddbsh browser bundle (page, pinned AWS SDK, Node shim + tests) beside ddbsh.js/ddbsh.wasm")

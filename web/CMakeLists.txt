# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# web/CMakeLists.txt — the Emscripten/WASM web target (P6d).
#
# Builds ONE WebAssembly module (ddbsh.wasm + ddbsh.js, an ES6 module) that runs
# the SDK-free engine in a browser. It links only the two portable libraries —
# ddbsh::ddbsh (engine), ddbsh::wire (codec + WireClient), and ddbsh-cli-core
# (the terminal-free batch policy) — plus the Asyncify bridge
# (transport_browser.cpp) and the embind surface (bindings.cpp). It links NO AWS
# SDK, NO readline, NO CLI: the browser target handles no credentials and makes
# no network call (in-browser ExtendDB only), so no builder-security preflight
# applies (D-P6-8).
#
# Configured only under the Emscripten toolchain (emcmake). Guarded by
# DDBSH_BUILD_WEB (default OFF, §4.1) so the normal CI matrix needs no
# Emscripten.

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "DDBSH_BUILD_WEB requires the Emscripten toolchain. "
        "Configure with `emcmake cmake --preset wasm-release` "
        "(see web/README.md for the pinned emsdk setup).")
endif()

add_executable(ddbsh-web
               bindings.cpp
               transport_browser.cpp)

# The emitted files are ddbsh.js (ES6 module glue) + ddbsh.wasm.
set_target_properties(ddbsh-web PROPERTIES OUTPUT_NAME ddbsh)

# bindings.cpp includes "repl.hpp" (the shared batch runner) from cli/.
target_include_directories(ddbsh-web PRIVATE "${CMAKE_SOURCE_DIR}/cli")

target_link_libraries(ddbsh-web
    PRIVATE ddbsh::ddbsh ddbsh::wire ddbsh-cli-core ddbsh_warnings)

# Emscripten link flags (D-P6-12 Asyncify + embind + ES6 module):
#   -sASYNCIFY            suspend/resume the C++ stack across Module.submit await
#   --bind               embind (the DdbshSession class + RunResult value_object)
#   -sMODULARIZE/ES6     emit an ES6 module exporting a factory (createDdbsh)
#   -sEXPORT_NAME        the factory name the harness/UI imports
#   -sALLOW_MEMORY_GROWTH the engine's working set is input-dependent
#   -sEXPORTED_RUNTIME_METHODS  the UTF8 helpers the Asyncify bridge JS uses
# Pinned to emsdk 3.1.61 (see web/README.md); revisit flags on an emsdk bump.
target_link_options(ddbsh-web PRIVATE
    "-sASYNCIFY"
    "--bind"
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    "-sEXPORT_NAME=createDdbsh"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sEXPORTED_RUNTIME_METHODS=['UTF8ToString','stringToUTF8','lengthBytesUTF8']")

# Copy the vendored in-browser ExtendDB adapter (pinned @0.1.26; see
# vendor/extenddb/PROVENANCE.md) and the Node smoke harness next to the build output so `node web/harness.mjs`
# (run from the build dir) finds ddbsh.js, ddbsh.wasm, and the adapter together.
add_custom_command(TARGET ddbsh-web POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor" "$<TARGET_FILE_DIR:ddbsh-web>/vendor"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/harness.mjs" "$<TARGET_FILE_DIR:ddbsh-web>/harness.mjs"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/package.json" "$<TARGET_FILE_DIR:ddbsh-web>/package.json"
    COMMENT "Staging vendored ExtendDB adapter + Node smoke harness beside ddbsh.wasm")

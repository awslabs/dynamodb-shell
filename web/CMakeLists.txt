# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# web/CMakeLists.txt — the Emscripten/WASM web target (P6d; two editions P6f).
#
# Builds the browser WebAssembly module(s) that run the SDK-free engine. The
# whole hard-to-port stack (lexer, parser, planner, executor, renderer,
# ddbsh-wire codec, WireClient) ships as ONE portable artifact; the only thing
# that differs between editions is the linked wire Transport (D-P6f-2):
#
#   ddbsh-wasm-extenddb-wasm  SubmitTransport  -> in-tab ExtendDB (Module.submit)
#                             OUTPUT_NAME ddbsh (ddbsh.js/ddbsh.wasm) — the
#                             zero-install, no-credential, no-network edition
#                             (safe to host publicly). This is the P6d/P6e path.
#
#   ddbsh-wasm                FetchTransport   -> ddbsh-web-proxy (Module.fetchDdb)
#                             OUTPUT_NAME ddbsh-wasm (ddbsh-wasm.js/.wasm) — the
#                             real-backend edition (real DynamoDB / real
#                             ExtendDB via the local signing proxy). The proxy
#                             and the final UI wiring land in P6f-b..e; P6f-a
#                             proves the edition builds and round-trips against
#                             a Node stub (harness-fetch.mjs).
#
# (The OUTPUT_NAME split keeps the existing P6e UI + Node smoke — which import
# ./ddbsh.js and wire Module.submit — green unchanged; the final ddbsh vs
# ddbsh-extenddb filename convention + UI edition-selection is a P6f-e concern.)
#
# Both editions link only the portable libraries — ddbsh::ddbsh (engine),
# ddbsh::wire (codec + WireClient), ddbsh-cli-core (the terminal-free batch
# policy) — plus the edition's Asyncify transport bridge and the shared embind
# surface (bindings.cpp). They link NO AWS SDK, NO readline, NO CLI. The fetch
# edition handles no credentials itself (the proxy signs); the submit edition
# makes no network call at all.
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

# The shared Emscripten link flags (D-P6-12 Asyncify + embind + ES6 module):
#   -sASYNCIFY            suspend/resume the C++ stack across the submit/fetch await
#   --bind               embind (the DdbshSession class + RunResult value_object)
#   -sMODULARIZE/ES6     emit an ES6 module exporting a factory (createDdbsh)
#   -sEXPORT_NAME        the factory name the harness/UI imports
#   -sALLOW_MEMORY_GROWTH the engine's working set is input-dependent
#   -sEXPORTED_RUNTIME_METHODS  the UTF8 helpers the Asyncify bridges' JS uses
# Pinned to the emsdk documented in web/README.md; revisit flags on an emsdk bump.
set(DDBSH_WEB_LINK_OPTIONS
    "-sASYNCIFY"
    "--bind"
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    "-sEXPORT_NAME=createDdbsh"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sEXPORTED_RUNTIME_METHODS=['UTF8ToString','stringToUTF8','lengthBytesUTF8']")

# One edition = the shared embind surface (bindings.cpp) + exactly one transport
# bridge TU. `fetch_edition` toggles DDBSH_WEB_FETCH (bindings.cpp picks the
# transport type) and the transport bridge compiled in.
function(ddbsh_add_web_edition target output_name transport_src fetch_edition)
    add_executable(${target} bindings.cpp ${transport_src})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${output_name})
    # bindings.cpp includes "repl.hpp" (the shared batch runner) from cli/.
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/cli")
    target_link_libraries(${target}
        PRIVATE ddbsh::ddbsh ddbsh::wire ddbsh-cli-core ddbsh_warnings)
    target_link_options(${target} PRIVATE ${DDBSH_WEB_LINK_OPTIONS})
    if(fetch_edition)
        target_compile_definitions(${target} PRIVATE DDBSH_WEB_FETCH)
    endif()
endfunction()

# The in-tab ExtendDB edition (OUTPUT_NAME ddbsh -> ddbsh.js/ddbsh.wasm).
ddbsh_add_web_edition(ddbsh-wasm-extenddb-wasm ddbsh transport_browser.cpp OFF)
# The real-backend proxy edition (OUTPUT_NAME ddbsh-wasm -> ddbsh-wasm.js/.wasm).
ddbsh_add_web_edition(ddbsh-wasm ddbsh-wasm transport_fetch.cpp ON)

# Stage ddbsh's OWN files beside the emitted modules so the output dir is BOTH
# (a) a Node-smoke environment and (b) a directly-servable static web bundle.
#
# The in-browser ExtendDB adapter (vendor/extenddb/) is DELIBERATELY NOT staged
# here (D-P6f-8): ddbsh never ships/commits the adapter blob, and the build
# stages only ddbsh's own files. The adapter is a third-party drop-in for the
# in-tab edition only — dropped into <bundle>/vendor/extenddb/ by
# devtools/stage-extenddb-webasm (run-web-smoke / serve-web) or by the
# user/deploy (see web/vendor/extenddb/PROVENANCE.md). This makes an adapter
# upgrade a swap-the-dir-and-relaunch, with no ddbsh rebuild.
#
# Staged files:
#   - harness.mjs  the P6d Node end-to-end smoke (SubmitTransport, in-tab ExtendDB),
#   - harness-fetch.mjs  the P6f-a Node smoke (FetchTransport over a stub proxy),
#   - harness-proxy.mjs  the P6f-b Node smoke (FetchTransport over the REAL
#                        ddbsh-web-proxy against real DynamoDB; run by
#                        devtools/run-proxy-smoke),
#   - harness-extenddb.mjs  the P6f-c Node smoke (FetchTransport over the REAL
#                        ddbsh-web-proxy against a REAL local ExtendDB server;
#                        run by devtools/run-extenddb-smoke),
#   - harness-proxy-lib.mjs  the shared handshake + corpus both proxy smokes use,
#   - package.json marks the output as ES modules for Node,
#   - index.html / app.js / app-core.mjs / styles.css  the P6e product UI (O-5),
#   - app-core.test.mjs  the DOM-free app-core Node unit tests (D-P6e-3).
# Staging is attached to the in-tab edition and ordered AFTER the fetch edition
# (add_dependencies) so both modules exist in the dir before the bundle is
# considered complete. After `cmake --build`, this dir holds ddbsh's complete
# static bundle; dropping the ExtendDB adapter into vendor/extenddb/
# (devtools/stage-extenddb-webasm) completes the in-tab edition (README).
add_dependencies(ddbsh-wasm-extenddb-wasm ddbsh-wasm)
add_custom_command(TARGET ddbsh-wasm-extenddb-wasm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/harness.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/harness-fetch.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/harness-proxy.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/harness-extenddb.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/harness-proxy-lib.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/package.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/app.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/app-core.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/app-core.test.mjs"
            "${CMAKE_CURRENT_SOURCE_DIR}/styles.css"
            "$<TARGET_FILE_DIR:ddbsh-wasm-extenddb-wasm>/"
    COMMENT "Staging ddbsh's own Node smokes + P6e web UI bundle beside the wasm modules (the ExtendDB adapter is a drop-in; see web/vendor/extenddb/PROVENANCE.md)")

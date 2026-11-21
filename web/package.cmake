# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# web/package.cmake — the browser edition's packaging step (P10, D-P10-7, G4).
#
# Run in CMake script mode by the ddbsh-web-package target (web/CMakeLists.txt):
#
#   cmake --build build-preset/wasm-release --target ddbsh-web-package
#
# It copies exactly the files listed in web/bundle-files.txt from the staged
# build output into a clean DIST_DIR and writes three things there:
#
#   <BUNDLE_NAME>/       the files to publish, nothing else
#   <BUNDLE_NAME>.zip    the same folder as a zip
#   SHA256SUMS           sha256 of every file in the folder and of the zip, in
#                        the `sha256sum -c` format (hash, two spaces, path
#                        relative to DIST_DIR)
#
# DIST_DIR is removed and recreated on every run, so it never holds files from
# an earlier version. Every file is written fresh (file(COPY_FILE) does not keep
# the source timestamp), which `aws s3 sync` relies on: it uploads a file when
# the local copy is newer than the object in the bucket.
#
# Inputs (-D): WEB_OUT (staged build output), MANIFEST (bundle-files.txt),
# DIST_DIR (output directory), BUNDLE_NAME (ddbsh-web-<version>).
# Portable: CMake only, no shell, so it runs the same on Linux, macOS and
# Windows.

cmake_minimum_required(VERSION 3.21)

foreach(var IN ITEMS WEB_OUT MANIFEST DIST_DIR BUNDLE_NAME)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "package.cmake: -D${var}=... is required")
    endif()
endforeach()

# Read the allowlist: one relative path per line; # comments and blanks skipped.
# ENCODING UTF-8: without it file(STRINGS) splits a line at any non-ASCII byte.
file(STRINGS "${MANIFEST}" manifest_lines ENCODING UTF-8)
set(bundle_files "")
foreach(line IN LISTS manifest_lines)
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES "^#")
        continue()
    endif()
    if(line MATCHES "^/" OR line MATCHES "\\\\" OR line MATCHES "(^|/)\\.\\.?(/|$)" OR line MATCHES " ")
        message(FATAL_ERROR "package.cmake: ${MANIFEST}: not a plain relative path: '${line}'")
    endif()
    if(line IN_LIST bundle_files)
        message(FATAL_ERROR "package.cmake: ${MANIFEST}: duplicate entry '${line}'")
    endif()
    list(APPEND bundle_files "${line}")
endforeach()
if(bundle_files STREQUAL "")
    message(FATAL_ERROR "package.cmake: ${MANIFEST} lists no files")
endif()

set(bundle_dir "${DIST_DIR}/${BUNDLE_NAME}")
file(REMOVE_RECURSE "${DIST_DIR}")
file(MAKE_DIRECTORY "${bundle_dir}")

set(sums "")
set(zip_members "")
foreach(rel IN LISTS bundle_files)
    set(src "${WEB_OUT}/${rel}")
    if(NOT EXISTS "${src}" OR IS_DIRECTORY "${src}")
        message(FATAL_ERROR "package.cmake: ${src} is missing; build the wasm-release preset first")
    endif()
    set(dst "${bundle_dir}/${rel}")
    get_filename_component(dst_dir "${dst}" DIRECTORY)
    file(MAKE_DIRECTORY "${dst_dir}")
    file(COPY_FILE "${src}" "${dst}" RESULT copy_result)
    if(NOT copy_result EQUAL 0)
        message(FATAL_ERROR "package.cmake: copying ${src}: ${copy_result}")
    endif()
    file(SHA256 "${dst}" hash)
    string(APPEND sums "${hash}  ${BUNDLE_NAME}/${rel}\n")
    list(APPEND zip_members "${BUNDLE_NAME}/${rel}")
endforeach()

# The zip holds the folder (entries are <BUNDLE_NAME>/<path>), in allowlist order.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${BUNDLE_NAME}.zip" --format=zip ${zip_members}
    WORKING_DIRECTORY "${DIST_DIR}"
    RESULT_VARIABLE zip_result)
if(NOT zip_result EQUAL 0)
    message(FATAL_ERROR "package.cmake: creating ${BUNDLE_NAME}.zip failed (${zip_result})")
endif()
file(SHA256 "${DIST_DIR}/${BUNDLE_NAME}.zip" zip_hash)
string(APPEND sums "${zip_hash}  ${BUNDLE_NAME}.zip\n")

file(WRITE "${DIST_DIR}/SHA256SUMS" "${sums}")
list(LENGTH bundle_files n)
message(STATUS "Packaged ${n} files: ${bundle_dir}, ${BUNDLE_NAME}.zip, SHA256SUMS")

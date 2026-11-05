# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# Standalone CMake project for the ddbsh OFFLINE embedding sample (D29).
#
# A DELIBERATELY EXTERNAL consumer of the installed find_package(ddbsh)
# package. Unlike the live sample it links ONLY `ddbsh::ddbsh` (the SDK-free
# engine) — no AWS SDK, no credentials — and drives the embedding API against
# the in-memory ddbsh::testing::MemoryClient. It builds and runs anywhere.
#
#     cmake -S examples/ddbshlib_inmemory_sample -B build-inmem \
#         -DCMAKE_PREFIX_PATH="/path/to/ddbsh/install"
#     cmake --build build-inmem
#     ./build-inmem/ddbshlib_inmemory_sample   # offline, zero cost

cmake_minimum_required(VERSION 3.21)

project(ddbshlib_inmemory_sample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ddbsh CONFIG REQUIRED)

add_executable(ddbshlib_inmemory_sample ddbshlib_inmemory_sample.cpp)
target_link_libraries(ddbshlib_inmemory_sample PRIVATE ddbsh::ddbsh)

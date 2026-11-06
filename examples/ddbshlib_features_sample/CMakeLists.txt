# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# Standalone CMake project for the ddbsh Phase-5 FEATURE embedding sample (D9).
#
# A DELIBERATELY EXTERNAL consumer of the installed find_package(ddbsh)
# package. Like the offline sample it links ONLY `ddbsh::ddbsh` (the SDK-free
# engine) — no AWS SDK, no credentials — and drives the Phase-5 embedding
# surface (prepared statements, typed set/binary bind, lazy streaming,
# transactions) against the in-memory ddbsh::testing::MemoryClient. It builds
# and runs anywhere, offline and at zero cost.
#
#     cmake -S examples/ddbshlib_features_sample -B build-features \
#         -DCMAKE_PREFIX_PATH="/path/to/ddbsh/install"
#     cmake --build build-features
#     ./build-features/ddbshlib_features_sample   # offline, zero cost

cmake_minimum_required(VERSION 3.21)

project(ddbshlib_features_sample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ddbsh CONFIG REQUIRED)

add_executable(ddbshlib_features_sample ddbshlib_features_sample.cpp)
target_link_libraries(ddbshlib_features_sample PRIVATE ddbsh::ddbsh)

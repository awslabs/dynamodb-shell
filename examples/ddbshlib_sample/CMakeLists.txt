# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# Standalone CMake project for the ddbsh LIVE embedding sample (D28).
#
# A DELIBERATELY EXTERNAL consumer of the installed find_package(ddbsh)
# package — it proves a third party can build their own application on the
# embedding API (ddbsh::aws::open) with nothing but `find_package(ddbsh)`. It
# links `ddbsh::aws`, which pulls `ddbsh::ddbsh` (the SDK-free engine) and the
# AWS SDK for C++.
#
# Build against an install prefix containing BOTH the ddbsh package and the
# AWS SDK:
#
#     cmake -S examples/ddbshlib_sample -B build-sample \
#         -DCMAKE_PREFIX_PATH="/path/to/ddbsh/install;/path/to/aws-sdk-cpp"
#     cmake --build build-sample
#     ./build-sample/ddbshlib_sample   # talks to REAL DynamoDB — costs money

cmake_minimum_required(VERSION 3.21)

project(ddbshlib_sample LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ddbsh CONFIG REQUIRED)
find_package(AWSSDK REQUIRED COMPONENTS dynamodb)

add_executable(ddbshlib_sample ddbshlib_sample.cpp)
target_link_libraries(ddbshlib_sample PRIVATE ddbsh::aws)

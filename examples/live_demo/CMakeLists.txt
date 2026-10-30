# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# Standalone CMake project for the ddbsh LIVE embedding sample.
#
# This is a DELIBERATELY EXTERNAL consumer of the installed find_package(ddbsh)
# package — NOT part of the project's own build. It proves a third party can
# build their own application on libddbsh + the ready-made AWS client with
# nothing but `find_package(ddbsh)`.
#
# Unlike the offline embed_demo (a bare Makefile, SDK-free), this sample needs
# the AWS SDK's transitive dependency graph resolved, which a hand-written
# Makefile cannot do — hence CMake. It links `ddbsh::aws`, which pulls
# `ddbsh::ddbsh` (the SDK-free engine) and the AWS SDK for C++.
#
# Build it against an install prefix that contains BOTH the ddbsh package and
# the AWS SDK (point CMAKE_PREFIX_PATH at them):
#
#     cmake -S examples/live_demo -B build-live \
#         -DCMAKE_PREFIX_PATH="/path/to/ddbsh/install;/path/to/aws-sdk-cpp"
#     cmake --build build-live
#     ./build-live/live_demo        # talks to REAL DynamoDB — costs real money

cmake_minimum_required(VERSION 3.21)

project(ddbsh_live_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The ddbsh package. Its config auto-resolves AWSSDK (find_dependency) so
# ddbsh::aws's AWS SDK interface links are satisfied; we also find_package it
# explicitly for the reader (and so a consumer who wants control over which SDK
# install is used sees exactly where it is resolved).
find_package(ddbsh CONFIG REQUIRED)
find_package(AWSSDK REQUIRED COMPONENTS dynamodb)

add_executable(live_demo live_demo.cpp)
target_link_libraries(live_demo PRIVATE ddbsh::aws)

# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# python/CMakeLists.txt — the `ddbsh._core` extension module (P13 T3,
# D-P13-8).
#
# Added by the top-level CMakeLists.txt when DDBSH_BUILD_PYTHON is ON, which
# the repo-root pyproject.toml sets for scikit-build-core together with
# DDBSH_BUILD_AWS=OFF, DDBSH_BUILD_TESTS=OFF and BUILD_SHARED_LIBS=OFF: the
# wheel holds the SDK-free engine and the ddbsh-wire codec, linked statically
# into one extension module, and no AWS SDK for C++. The Python package
# itself (python/ddbsh/) is pure Python; scikit-build-core copies it into the
# wheel (wheel.packages) and this file installs only the module, as the
# install component `python` (install.components), so the engine's own
# install rules (headers, libraries, the CMake package) stay out of the wheel.

find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module)

# nanobind is a build-time dependency (pyproject.toml build-system.requires).
execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE nanobind_ROOT
    RESULT_VARIABLE nanobind_rc)
if(NOT nanobind_rc EQUAL 0)
    message(FATAL_ERROR "DDBSH_BUILD_PYTHON needs nanobind: pip install nanobind")
endif()
find_package(nanobind CONFIG REQUIRED)

# NB_STATIC links nanobind's own runtime into the module (one module, no
# shared libnanobind to ship).
nanobind_add_module(_core NB_STATIC src/bindings.cpp)
target_include_directories(_core PRIVATE "${PROJECT_SOURCE_DIR}/cli")
target_link_libraries(_core PRIVATE ddbsh::ddbsh ddbsh::wire ddbsh-cli-core ddbsh_warnings)

install(TARGETS _core LIBRARY DESTINATION ddbsh COMPONENT python)

# Copyright 2026 ddbsh reimplementation contributors.
# SPDX-License-Identifier: Apache-2.0
#
# Plain Makefile for the ddbsh library embedding sample.
#
# This is a DELIBERATELY STANDALONE Makefile — NOT the project's CMake. It
# demonstrates that an external consumer can build against the installed public
# library (headers under $(DDBSH_PREFIX)/include/ddbsh, lib under
# $(DDBSH_PREFIX)/lib) with nothing but a C++23 compiler. It links only
# `-lddbsh`; the engine's public headers are SDK-free, so no AWS SDK is needed
# to embed the library (this offline sample supplies its own IDDBClient).
#
# Build the library + install it to a prefix first, e.g.:
#
#     cmake -S . -B build
#     cmake --build build
#     cmake --install build --prefix /path/to/prefix
#
# Then build and run this sample against that prefix:
#
#     make DDBSH_PREFIX=/path/to/prefix
#     make run DDBSH_PREFIX=/path/to/prefix
#
# DDBSH_PREFIX defaults to /usr/local (a system-wide install). Override it on
# the command line (as above) or in the environment.

DDBSH_PREFIX ?= /usr/local

CXX      ?= c++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -O2
CPPFLAGS += -I$(DDBSH_PREFIX)/include
# -rpath so the runtime loader finds libddbsh.so without LD_LIBRARY_PATH.
LDFLAGS  += -L$(DDBSH_PREFIX)/lib -Wl,-rpath,$(DDBSH_PREFIX)/lib
LDLIBS   += -lddbsh

TARGET := embed_demo
SRC    := embed_demo.cpp

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(SRC) -o $@ $(LDFLAGS) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

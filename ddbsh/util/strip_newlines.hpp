// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * strip_newlines.hpp
 *
 * simple utility function to strip newlines from an std::string
 *
 * Amrith - 2022-11-05
 */


#ifndef __STRIP_NEWLINES_HPP_DEFINED__
#define __STRIP_NEWLINES_HPP_DEFINED__

#include <algorithm>
#include <string>

std::string strip_newlines(std::string s);
std::string explain_string(std::string s);

#endif

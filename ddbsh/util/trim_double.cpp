// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * trim-double.cpp
 *
 * provided the string form of a double, trim trailing zeroes.
 *
 * Amrith - 2022-10-28
 */

#include "trim_double.hpp"

std::string trim_double(std::string input)
{
    std::regex reg("^([\\d,]+)$|^([\\d,]+)\\.0*$|^([\\d,]+\\.[0-9]*?)0*$");

    return std::regex_replace(input, reg, "$1$2$3");
}

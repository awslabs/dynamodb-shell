// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * version.cpp
 *
 * Construct a version string
 *
 * Amrith. 2023-01-27
 *
 */

#include "version.h"
#include "stdio.h"
#include "version.hpp"

#define VERSION_STRING_LENGTH (64)
static char ddbsh_version_string[VERSION_STRING_LENGTH+1];

char * ddbsh_version()
{
    if (VERSION_TWEAK > 0)
        snprintf(ddbsh_version_string, VERSION_STRING_LENGTH, "%d.%d.%d.%d",
                 VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TWEAK);
    else if (VERSION_PATCH > 0)
        snprintf(ddbsh_version_string, VERSION_STRING_LENGTH, "%d.%d.%d",
                 VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
    else
        snprintf(ddbsh_version_string, VERSION_STRING_LENGTH, "%d.%d",
                 VERSION_MAJOR, VERSION_MINOR);

    return ddbsh_version_string;
}


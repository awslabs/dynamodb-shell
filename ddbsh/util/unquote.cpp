// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * unquote.cpp
 *
 * unquote a double quoted string.
 *
 * Amrith 2022-09-08
 */

#include "unquote.hpp"
#include <string.h>
#include "allocation.hpp"

/* always return an strdup'ed string */

char * unquote(const char * s)
{
    char * uq;
    int l = strlen(s);

    if (*s == '"' && s[l-1] == '"')
    {
        uq = STRDUP((char *) s+1);
        uq[l-2] = 0;
    }
    else
    {
        uq = STRDUP((char *)s);
    }

    return uq;
}

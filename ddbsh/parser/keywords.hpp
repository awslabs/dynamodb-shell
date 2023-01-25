// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * keywords.hpp
 *
 * handle all keywords - find a keyword (reserved word) and return its
 * corresponding token.
 *
 * Amrith - 2022-08-08.
 */

#ifndef __KEYWORDS_HPP_DEFINED__
#define __KEYWORDS_HPP_DEFINED__

#include "parser-defs.hpp"
#include "command.hpp"

using namespace ddbsh;
#include "parser.hpp"

typedef struct
{
    const char * keyword;
    int token;
    const char * token_string;
} keywords_t;

int lookup_keyword (char * text, int * ptoken);

#endif


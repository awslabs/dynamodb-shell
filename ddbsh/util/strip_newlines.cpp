// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * strip_newlines.cpp
 *
 * simple utility function to strip newlines from an std::string
 *
 * Amrith - 2022-11-05
 */

#include <algorithm>
#include <string>
#include "strip_newlines.hpp"
#include <regex>
#include <stdio.h>
#include <ctype.h>

static void hexdump(const char * s)
{
    int offset = 0;
    char linebuf[16];
    int linesz;

    do
    {
        // read 16 bytes at a time.
        for (linesz = 0; linesz < 16 && s[offset]; offset ++, linesz ++)
            linebuf[linesz] = s[offset];

        if (linesz == 0)
            break;

        printf("%08x |", offset);
        for (int ix = 0; ix < 16; ix ++)
        {
            if (ix % 4 == 0)
                putchar (' ');

            if (ix < linesz)
            {
                printf("%02x", linebuf[ix]);
            }
            else
            {
                printf ( "  " );
            }
        }

        printf ( " | " );

        for (int ix = 0; ix < 16; ix ++)
        {
            if (ix < linesz)
            {
                if (isprint(linebuf[ix]))
                    putchar (linebuf[ix]);
                else
                    putchar ('.');
            }
            else
            {
                putchar (' ');
            }
        }

        printf ( " |\n" );
    } while(linesz == 16);
}

static std::regex whitespace("[\n\r][\t]*");
static std::regex colontab(":\t");

std::string strip_newlines(std::string s)
{
    std::string rv = std::regex_replace(std::regex_replace(s, whitespace, " "), colontab, ": ");
    // hexdump(s.c_str());
    // hexdump(rv.c_str());
    return rv;
}

static std::regex tabs("[\t]");
std::string explain_string(std::string s)
{
    std::string rv = std::regex_replace(s, tabs, "   ");
    // hexdump(s.c_str());
    // hexdump(rv.c_str());
    return rv;

}

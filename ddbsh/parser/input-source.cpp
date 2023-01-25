// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * input-source.cpp
 *
 * definitions related to the input source (stdin, file, or string)
 *
 * Amrith - 2022-11-19
 */

#include "ddbsh.hpp"
#include "input-source.hpp"
#include <readline/history.h>
#include <readline/readline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "allocation.hpp"

#define YY_NULL 0
extern "C" FILE* yyin;

int (*readinput)(char * buffer, int size);

using namespace ddbsh;

static int readinputstdin(char * buffer, int size)
{
     char * line;
     char prompt[32+2+1];

     logdebug("[%s, %d] %s, size = %d\n", __FILENAME__, __LINE__, __FUNCTION__, size);
     if (feof(yyin))
         return YY_NULL;

     strncpy(prompt, CDDBSh::getInstance()->getRegion().c_str(), sizeof(prompt)-3);
     strcat(prompt, "> ");

     if ((line = readline(prompt)) == NULL)
         return YY_NULL;

     if (strlen(line) > size - 2)
     {
         FREE(line);
         logerror("[%s, %d] Input line is too long, max length is %d.\n",
                  __FILENAME__, __LINE__, size - 2);
         return YY_NULL;
     }

     if(strlen(line))
     {
         add_history(line);
     }

     strcpy(buffer, line);
     strcat(buffer, "\n");
     FREE(line);
     return (strlen(buffer));
}

static FILE * finput;
static int readinputfile(char * buffer, int size)
{
    int ix = 0;

    logdebug("[%s, %d] %s, size = %d\n", __FILENAME__, __LINE__, __FUNCTION__, size);
    while(!feof(finput) && ix < size - 1)
    {
        buffer[ix++] = fgetc(finput);
    }

    buffer[ix] = 0;
    return(strlen(buffer));
}

typedef struct
{
    char * inputstring;
    int offset;
    int length;
} input_string_t;

static input_string_t inputstring;

static int readinputstring(char * buffer, int size)
{
    logdebug("[%s, %d] %s, size = %d\n", __FILENAME__, __LINE__, __FUNCTION__, size);
    if (inputstring.inputstring == NULL)
        return YY_NULL;

    if (size - 2 > inputstring.length - inputstring.offset)
    {
        strcpy(buffer, inputstring.inputstring + inputstring.offset);
        inputstring.offset = inputstring.length = 0;
        FREE(inputstring.inputstring);
        inputstring.inputstring = NULL;
        if (buffer[strlen(buffer)-1] != ';')
            strcat(buffer, ";");
    }
    else
    {
        strncpy(buffer, inputstring.inputstring + inputstring.offset, size - 1);
        inputstring.offset += (size - 1);
    }

    return (strlen(buffer));

}

void setinputstring(char *  str)
{
    readinput = readinputstring;
    if (inputstring.inputstring)
        FREE(inputstring.inputstring);

    inputstring.inputstring = str;
    inputstring.offset = 0;
    inputstring.length = strlen(str);
}

void setinputfile(FILE * fp)
{
    finput = fp;
    readinput = readinputfile;
}

void setinputstdin()
{
    readinput = readinputstdin;
}

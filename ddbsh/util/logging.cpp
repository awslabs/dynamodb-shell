// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * logging.c
 *
 * All error logging and debugging routines.
 *
 * Amrith Kumar
 */

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "logging.hpp"
#include <unistd.h>

static FILE * __logging_fp = NULL;
static int __debug_level = DDBSH_LOGLEVEL_DEFAULT;

#define LOGGING_TIMESTRING_BUFFER_SIZE (64)

static int isenabled (int level)
{
    if (__debug_level >= level)
	return 1;
    else
	return 0;
}

static char * timestring (char * string)
{
    struct tm tm;
    time_t tt;

    if (string == NULL)
	string = (char *) malloc (LOGGING_TIMESTRING_BUFFER_SIZE+1);

    time(&tt);
    localtime_r (&tt, &tm);

    snprintf ( string, LOGGING_TIMESTRING_BUFFER_SIZE, "%4d-%02d-%02d %02d:%02d:%02d (%lu)",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
               tm.tm_hour, tm.tm_min, tm.tm_sec, (unsigned long) getpid());

    return (string);
}

static int __logdebug (const char * prefix,
                       const char * string, va_list ap)
{
    char buffer[LOGGING_TIMESTRING_BUFFER_SIZE+1];
    int rv;

    if (__logging_fp == NULL)
	__logging_fp = stderr;

    timestring(buffer);

    fprintf ( __logging_fp, "%s [%s] ", buffer, prefix );
    rv = vfprintf ( __logging_fp, string, ap );
    fflush(__logging_fp);

    return rv;
}

int logdebug(const char * string, ...)
{
    va_list ap;
    int rv;

    if (!isenabled(DDBSH_LOGLEVEL_DEBUG))
	return 0;

    va_start (ap, string);

    rv = __logdebug((const char * ) "DEBUG",
                    string, ap);

    va_end (ap);
    return rv;
}

int logverbose(const char * string, ...)
{
    va_list ap;
    int rv;

    if (!isenabled(DDBSH_LOGLEVEL_VERBOSE))
	return 0;

    va_start (ap, string);

    rv = __logdebug((const char *) "VERBOSE", string, ap);

    va_end (ap);
    return rv;
}

int loginfo(const char * string, ...)
{
    va_list ap;
    int rv;

    if (!isenabled(DDBSH_LOGLEVEL_INFO))
	return 0;

    va_start (ap, string);

    rv = __logdebug((const char *) "INFO", string, ap);

    va_end (ap);
    return rv;
}

int logmsg(const char * string, ...)
{
    va_list ap;
    int rv;

    if (!isenabled(DDBSH_LOGLEVEL_DEFAULT))
	return 0;

    va_start (ap, string);

    rv = __logdebug((const char *) "MESSAGE", string, ap);

    va_end (ap);

    va_start (ap, string);
    vfprintf(stdout, string, ap);
    fflush(stderr);
    va_end (ap);

    return rv;
}

int logerror (const char * string, ...)
{
    va_list ap;
    int rv;

    if (!isenabled(DDBSH_LOGLEVEL_ERROR))
	return 0;

    va_start (ap, string);

    rv = __logdebug((const char *) "ERROR", string, ap);
    va_end (ap);

    va_start (ap, string);
    vfprintf(stderr, string, ap);
    fflush(stderr);
    va_end (ap);
    return rv;
}


void setdebug (int level)
{
    __debug_level = level;
}

void cleardebug ()
{
    __debug_level = 0;
}

int logtofile (char * fname)
{
    char __lfname[128+1];
    if (__logging_fp && (__logging_fp != stderr))
    {
	fclose(__logging_fp);
    }

    if (fname == NULL)
    {
        strcpy(__lfname, "/tmp/ddbsh-debug-log.txt");
        fname = __lfname;
    }

    if ((__logging_fp = fopen ( fname, "a+" )) == (FILE *) NULL)
    {
	__logging_fp = stderr;
	return 0;
    }

    logdebug("[%s, %d] Opening logfile.\n", __FILENAME__, __LINE__);
    return 1;
}



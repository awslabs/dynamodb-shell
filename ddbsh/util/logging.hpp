// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
#ifndef __LOGGING_HPP_DEFINED__
#define __LOGGING_HPP_DEFINED__

int logtofile (char * fname);

void setdebug (int level);

void cleardebug ();

int logerror (const char * string, ...);
int logmsg(const char * string, ...);
int loginfo(const char * string, ...);
int logverbose(const char * string, ...);
int logdebug(const char * string, ...);

#define DDBSH_LOGLEVEL_DEBUG    0x8000
#define DDBSH_LOGLEVEL_VERBOSE  0x0800
#define DDBSH_LOGLEVEL_INFO     0x0080
#define DDBSH_LOGLEVEL_NORMAL   0x0008
#define DDBSH_LOGLEVEL_ERROR    0x0000

#define DDBSH_LOGLEVEL_DEFAULT DDBSH_LOGLEVEL_INFO

#include <string.h>

#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#endif

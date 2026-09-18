// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * input-source.hpp
 *
 * definitions related to the input source (stdin, file, or string)
 *
 * Amrith - 2022-11-19
 */

#ifndef __INPUT_SOURCE_HPP_DEFINED__
#define __INPUT_SOURCE_HPP_DEFINED__

extern int (*readinput)(char * buffer, int size);
void setinputstring(char *  str);
void setinputfile(FILE * fp);
void setinputstdin();

#endif

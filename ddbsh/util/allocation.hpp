// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * allocation.hpp
 *
 * a simple memory allocator for leak detection.
 *
 * Amrith - 2022-12-06
 */

#ifdef TEST_HARNESS

#ifndef __ALLOCATION_HPP_DEFINED__
#define __ALLOCATION_HPP_DEFINED__

#include <iostream>
#include <stdlib.h>

void * intercept_malloc(const char *, int, size_t);
void * intercept_calloc(const char *, int, size_t, size_t);
void * intercept_realloc(const char *, int, void *, size_t );
char * intercept_strdup(const char *, int, char * );
void   intercept_free(const char *, int, void *);

#define MALLOC(x)     intercept_malloc(__FILE__, __LINE__, (x))
#define CALLOC(x,y)   intercept_calloc(__FILE__, __LINE__, (x), (y))
#define REALLOC(x,y)  intercept_realloc(__FILE__, __LINE__, (x), (y))
#define STRDUP(x)     intercept_strdup(__FILE__, __LINE__, (x))
#define FREE(x)       intercept_free(__FILE__, __LINE__, (x))

void show_allocations (void);
void get_allocations(int * a, int * b);

void * operator new(size_t size, const char *, int) throw (std::bad_alloc);
void * operator new[](size_t size, const char *, int) throw (std::bad_alloc);
void   operator delete(void *) throw ();
void   operator delete[](void *) throw();

#define NEW     new(__FILE__, __LINE__)

// it'd be really hard to replace delete :(

#endif

#else

#define MALLOC(x)     malloc((x))
#define CALLOC(x,y)   calloc((x), (y))
#define REALLOC(x,y)  realloc((x), (y))
#define STRDUP(x)     strdup((x))
#define FREE(x)       free((x))
#define NEW new

#endif

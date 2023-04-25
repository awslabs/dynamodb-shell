// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * allocation.c
 *
 * a simple memory allocator for leak detection.
 *
 * Amrith - 2022-12-06
 */

#include <assert.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ll.hpp"
#include "logging.hpp"

typedef struct
{
    ll_t list;

    void * block;
    char * src;
    unsigned int line;
    int sz;
} allocation_t;

typedef struct
{
    int allocations;
    int frees;
    ll_t allocation_chain;
} allocation_info_t;

static char * filename(const char * pathname)
{
    // returns an strduped filename which is just the file part of the
    // name of a file.
    int ix;
    for (ix = strlen(pathname); ix && pathname[ix] != '/'; ix --)
        ;

    if (pathname[ix] == '/')
        return strdup(pathname + ix + 1);
    else
        return strdup(pathname);
}

static allocation_info_t ai;

static void __allocation_init ()
{
    static int initialized = 0;

    if (!initialized)
    {
	ll_init (&ai.allocation_chain);
	initialized = 1;
    }
}

void get_allocations(int * a, int * b)
{
    __allocation_init();
    *a = ai.allocations;
    *b = ai.frees;
}

void * intercept_malloc(const char * file, int line, size_t sz)
{
    allocation_t * allocation;

    __allocation_init();

    if ((allocation = (allocation_t *) malloc(sizeof(allocation_t))) == NULL)
        return allocation;

    if ((allocation->block = (void *) malloc(sz)) == NULL)
    {
        free(allocation);
        return NULL;
    }

    allocation->src = filename(file);
    allocation->line = line;
    allocation->sz = sz;

    ll_thread_at_tail (&ai.allocation_chain, allocation, &allocation->list);
    ai.allocations ++;

    return allocation->block;
}

void * intercept_calloc(const char * file, int line, size_t nmemb, size_t sz)
{
    allocation_t * allocation;

    __allocation_init();

    if ((allocation = (allocation_t *) malloc(sizeof(allocation_t))) == NULL)
        return allocation;

    if ((allocation->block = (void *) malloc(nmemb * sz)) == NULL)
    {
        free(allocation);
        return NULL;
    }

    memset (allocation->block, 0, nmemb * sz);

    allocation->src = filename(file);
    allocation->line = line;
    allocation->sz = nmemb * sz;

    ll_thread_at_tail (&ai.allocation_chain, allocation, &allocation->list);
    ai.allocations ++;

    return allocation->block;
}

static allocation_t * find_alloc(void * p)
{
    __allocation_init();

    allocation_t * node;

    for (node = (allocation_t *) ai.allocation_chain.next; node; node = (allocation_t *)node->list.next)
    {
        if (node->block == p)
            return node;
    }

    return node;
}

void * intercept_realloc(const char * file, int line, void * ptr, size_t sz )
{
    __allocation_init();

    allocation_t * header;

    if (ptr == NULL)
        return intercept_malloc(file, line, sz);

    if ((header = find_alloc(ptr)) == NULL)
        return realloc(ptr, sz);

    free(header->src);
    header->src = filename(file);
    header->line = line;
    header->sz = sz;
    header->block = realloc(ptr, sz);

    return (header->block);
}

void intercept_free (const char * file, int line, void * ptr)
{
    __allocation_init();

    allocation_t * header;
    if (ptr == NULL)
        return;

    if ((header = find_alloc(ptr)) == NULL)
    {
        free(ptr);
        return;
    }

    ll_unthread (&header->list);
    free (header->src);
    free (header);

    ai.frees ++;

    free(ptr);

    return;
}

char * intercept_strdup (const char * file, int line, char * ptr )
{
    allocation_t * allocation;

    __allocation_init();

    if ((allocation = (allocation_t *) malloc(sizeof(allocation_t))) == NULL)
        return NULL;

    if ((allocation->block = strdup(ptr)) == NULL)
    {
        free(allocation);
        return NULL;
    }

    allocation->src = filename(file);
    allocation->line = line;
    allocation->sz = strlen(allocation->src) + 1;

    ll_thread_at_tail (&ai.allocation_chain, allocation, &allocation->list);
    ai.allocations ++;

    return (char *) allocation->block;
}

void show_allocations (void)
{
    allocation_t * h;
    __allocation_init();

    logerror ( "------------------------------------------------------------\n");
    logerror ( "Allocations = %d, Frees = %d\n", ai.allocations, ai.frees);

    for (h = (allocation_t *) ai.allocation_chain.next; h; h = (allocation_t *) h->list.next)
	logerror ( "Allocation hdr @%p, block @%p: %s(%d) sz = %d\n", h, h->block, h->src, h->line, h->sz );

    logerror ( "------------------------------------------------------------\n");
}

void * operator new(size_t size, const char * file, int line) throw(std::bad_alloc)
{
    return intercept_malloc(file, line, size);
}

void * operator new[](size_t size, const char * file, int line) throw(std::bad_alloc)
{
    return intercept_malloc(file, line, size);
}

void operator delete(void * ptr) throw()
{
    intercept_free(NULL, 0, ptr);
}

void operator delete[](void * ptr) throw()
{
    intercept_free(NULL, 0, ptr);
}

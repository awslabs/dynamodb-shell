// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * ll.c - simple linked list manager.
 *
 * Amrith, 2022-05-13
 *
 */

#include <stdio.h>
#include "ll.hpp"

void ll_init ( ll_t * h )
{
    h->next = h->prev = NULL;
    h->nextp = h->prevp = h;
}

void ll_thread_at_head ( ll_t * h, void * node, ll_t * links )
{
    ll_t * n = (ll_t *) h->nextp;

    links->prev = NULL;
    links->prevp = h;
    links->next = h->next;
    links->nextp = h->nextp;

    n->prev = node;
    n->prevp = links;
    h->next = node;
    h->nextp = links;
}

void ll_thread_at_tail ( ll_t * h, void * node, ll_t * links )
{
    ll_t * p = (ll_t *) h->prevp;

    links->next = NULL;
    links->nextp = h;
    links->prev = h->prev;
    links->prevp = h->prevp;

    p->next = node;
    p->nextp = links;
    h->prev = node;
    h->prevp = links;
}

void ll_thread_before ( ll_t * n, void * node, ll_t * links )
{
    ll_t * p = (ll_t *) n->prevp;

    links->next = p->next;
    links->nextp = p->nextp;
    links->prev = n->prev;
    links->prevp = n->prevp;

    p->next = node;
    p->nextp = links;

    n->prev = node;
    n->prevp = links;
}

void ll_thread_after ( ll_t * n, void * node, ll_t * links )
{
    ll_t * p = (ll_t *) n->nextp;

    links->next = n->next;
    links->nextp = n->nextp;
    links->prev = p->prev;
    links->prevp = p->prevp;

    n->next = node;
    n->nextp = links;

    p->prev = node;
    p->prevp = links;
}

void ll_unthread ( ll_t * links )
{
    ll_t * p, * n;

    p = (ll_t *) links->prevp;
    n = (ll_t *) links->nextp;

    p->next = links->next;
    p->nextp = links->nextp;
    n->prev = links->prev;
    n->prevp = links->prevp;
}

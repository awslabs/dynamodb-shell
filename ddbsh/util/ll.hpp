// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * ll.h - simple linked list manager.
 *
 * Amrith, 2022-05-13
 *
 */

#ifndef __LL_H_DEFINED__
#define __LL_H_DEFINED__

typedef struct
{
    void * next;
    void * nextp;
    void * prev;
    void * prevp;
} ll_t;

void ll_init ( ll_t * h );
void ll_thread_at_head ( ll_t * h, void * node, ll_t * links );
void ll_thread_at_tail ( ll_t * h, void * node, ll_t * links );
void ll_thread_before ( ll_t * n, void * node, ll_t * links );
void ll_thread_after ( ll_t * n, void * node, ll_t * links );
void ll_unthread ( ll_t * links );

#endif

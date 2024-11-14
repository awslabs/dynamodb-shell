// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * keywords.cpp
 *
 * handle all keywords - find a keyword (reserved word) and return its
 * corresponding token.
 *
 * Amrith - 2022-08-08.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "keywords.hpp"
#include "logging.hpp"

/* WARNING: keywords_table is used for a binary search. If the
 * keywords are not alpha sorted, then the binary search won't work
 * well :)
 */

static keywords_t keywords_table[] = {
    { "access", K_ACCESS, "K_ACCESS" },
    { "add", K_ADD, "K_ADD" },
    { "aes256", K_AES256, "K_AES256" },
    { "all", K_ALL, "K_ALL" },
    { "alter", K_ALTER, "K_ALTER" },
    { "and", K_AND, "K_AND" },
    { "as", K_AS, "K_AS" },
    { "attribute_exists", K_ATTRIBUTE_EXISTS, "K_ATTRIBUTE_EXISTS" },
    { "attribute_not_exists", K_ATTRIBUTE_NOT_EXISTS, "K_ATTRIBUTE_NOT_EXISTS" },
    { "attribute_type", K_ATTRIBUTE_TYPE, "K_ATTRIBUTE_TYPE" },
    { "backup", K_BACKUP, "K_BACKUP" },
    { "backups", K_BACKUPS, "K_BACKUPS" },
    { "begin", K_BEGIN, "K_BEGIN" },
    { "begins_with", K_BEGINS_WITH, "K_BEGINS_WITH" },
    { "between", K_BETWEEN, "K_BETWEEN" },
    { "billing", K_BILLING, "K_BILLING" },
    { "binary", K_ATTRIBUTE_TYPE_BINARY, "K_ATTRIBUTE_TYPE_BINARY" },
    { "both", K_BOTH, "K_BOTH" },
    { "brief", K_BRIEF, "K_BRIEF" },
    { "called", K_CALLED, "K_CALLED" },
    { "capacity", K_CAPACITY, "K_CAPACITY" },
    { "check", K_CHECK, "K_CHECK" },
    { "class", K_CLASS, "K_CLASS" },
    { "commit", K_COMMIT, "K_COMMIT" },
    { "connect", K_CONNECT, "K_CONNECT" },
    { "consistent", K_CONSISTENT, "K_CONSISTENT" },
    { "consumed", K_CONSUMED, "K_CONSUMED" },
    { "contains", K_CONTAINS, "K_CONTAINS" },
    { "create", K_CREATE, "K_CREATE" },
    { "delete", K_DELETE, "K_DELETE" },
    { "deletion", K_DELETION, "K_DELETION" },
    { "demand", K_DEMAND, "K_DEMAND" },
    { "describe", K_DESCRIBE, "K_DESCRIBE" },
    { "disabled", K_DISABLED, "K_DISABLED" },
    { "drop", K_DROP, "K_DROP" },
    { "enabled", K_ENABLED, "K_ENABLED" },
    { "end", K_COMMIT, "K_COMMIT" },
    { "endpoint", K_ENDPOINT, "K_ENDPOINT" },
    { "exists", K_EXISTS, "K_EXISTS" },
    { "explain", K_EXPLAIN, "K_EXPLAIN" },
    { "false", K_FALSE, "K_FALSE" },
    { "from", K_FROM, "K_FROM" },
    { "global", K_GLOBAL, "K_GLOBAL" },
    { "gsi", K_GSI, "K_GSI" },
    { "hash", K_HASH, "K_HASH" },
    { "help", K_HELP, "K_HELP" },
    { "if", K_IF, "K_IF" },
    { "image", K_IMAGE, "K_IMAGE" },
    { "images", K_IMAGES, "K_IMAGES" },
    { "in", K_IN, "K_IN" },
    { "include", K_INCLUDE, "K_INCLUDE" },
    { "index", K_INDEX, "K_INDEX" },
    { "indexes", K_INDEXES, "K_INDEXES" },
    { "infrequent", K_INFREQUENT, "K_INFREQUENT" },
    { "insert", K_INSERT, "K_INSERT" },
    { "into", K_INTO, "K_INTO" },
    { "is", K_IS, "K_IS" },
    { "key", K_KEY, "K_KEY" },
    { "keyid", K_KEYID, "K_KEYID" },
    { "keys", K_KEYS, "K_KEYS" },
    { "kms", K_KMS, "K_KMS" },
    { "like", K_LIKE, "K_LIKE" },
    { "limit", K_LIMIT, "K_LIMIT" },
    { "limits", K_LIMITS, "K_LIMITS" },
    { "lsi", K_LSI, "K_LSI" },
    { "mode", K_MODE, "K_MODE" },
    { "new", K_NEW, "K_NEW" },
    { "not", K_NOT, "K_NOT" },
    { "nowait", K_NOWAIT, "K_NOWAIT" },
    { "null", K_NULL, "K_NULL" },
    { "number", K_ATTRIBUTE_TYPE_NUMBER, "K_ATTRIBUTE_TYPE_NUMBER" },
    { "numeric", K_NUMBER, "K_NUMBER" },
    { "old", K_OLD, "K_OLD" },
    { "on", K_ON, "K_ON" },
    { "only", K_ONLY, "K_ONLY" },
    { "or", K_OR, "K_OR" },
    { "pitr", K_PITR, "K_PITR" },
    { "primary", K_PRIMARY, "K_PRIMARY" },
    { "projecting", K_PROJECTING, "K_PROJECTING" },
    { "protection", K_PROTECTION, "K_PROTECTION" },
    { "provisioned", K_PROVISIONED, "K_PROVISIONED" },
    { "quit", K_QUIT, "K_QUIT" },
    { "range", K_RANGE, "K_RANGE" },
    { "ratelimit", K_RATELIMIT, "K_RATELIMIT" },
    { "rcu", K_RCU, "K_RCU" },
    { "remove", K_REMOVE, "K_REMOVE"},
    { "replace", K_REPLACE, "K_REPLACE" },
    { "replica", K_REPLICA, "K_REPLICA" },
    { "restore", K_RESTORE, "K_RESTORE" },
    { "return", K_RETURN, "K_RETURN" },
    { "rollback", K_ROLLBACK, "K_ROLLBACK" },
    { "select", K_SELECT, "K_SELECT" },
    { "set", K_SET, "K_SET" },
    { "show", K_SHOW, "K_SHOW" },
    { "size", K_SIZE, "K_SIZE" },
    { "sse", K_SSE, "K_SSE" },
    { "standard", K_STANDARD, "K_STANDARD" },
    { "stream", K_STREAM, "K_STREAM" },
    { "string", K_ATTRIBUTE_TYPE_STRING, "K_ATTRIBUTE_TYPE_STRING" },
    { "table", K_TABLE, "K_TABLE" },
    { "tables", K_TABLES, "K_TABLES" },
    { "tags", K_TAGS, "K_TAGS" },
    { "throughput", K_THROUGHPUT, "K_THROUGHPUT" },
    { "to", K_TO, "K_TO" },
    { "total", K_TOTAL, "K_TOTAL" },
    { "true", K_TRUE, "K_TRUE" },
    { "ttl", K_TTL, "K_TTL" },
    { "unlimited", K_UNLIMITED, "K_UNLIMITED" },
    { "update", K_UPDATE, "K_UPDATE" },
    { "updateD", K_UPDATED, "K_UPDATED" },
    { "upsert", K_UPSERT, "K_UPSERT" },
    { "values", K_VALUES, "K_VALUES" },
    { "version", K_VERSION, "K_VERSION" },
    { "warm", K_WARM, "K_WARM" },
    { "wcu", K_WCU, "K_WCU" },
    { "where", K_WHERE, "K_WHERE" },
    { "with", K_WITH, "K_WITH" },
};

/* lookup a keyword in the list of keywords. return 0 and set ptoken,
 * or return -1 if the token is not matched. */
/* FIXME: improve the search by replacing strcasecmp with memcmp, and
 * doing case conversion only once. */

int lookup_keyword (char * text, int * ptoken)
{
    int low, mid, high, cc;

    logdebug("[%s, %d] Looking up %s\n", __FILENAME__, __LINE__, text);

    low = 0; // low index
    high = sizeof(keywords_table)/sizeof(keywords_t) - 1; // high index

#define CHECK(ix) if (!(cc = strcasecmp(keywords_table[(ix)].keyword, text))) \
    {\
        *ptoken = keywords_table[(ix)].token;\
        logdebug("[%s, %d] Returning %s (%d) for keyword %s\n", \
                 __FILENAME__, __LINE__, keywords_table[(ix)].keyword,  \
                 ix, keywords_table[(ix)].token_string); \
        return 0; \
    }

    CHECK(low);

    CHECK(high);

    if (strcasecmp(keywords_table[low].keyword, text) < 0 &&
        strcasecmp(text, keywords_table[high].keyword) < 0)
    {
        while (low < high)
        {
            mid = (low + high)/2;

            CHECK(mid);

            /* cc is set in CHECK() */
            if (cc < 0)
            {
                if (low == mid)
                {
                    logdebug("[%s, %d] Returning -1 for keyword %s\n",
                             __FILENAME__, __LINE__, text);
                    return -1;
                }

                low = mid;
                continue;
            }
            else
            {
                if (high == mid)
                {
                    logdebug("[%s, %d] Returning -1 for keyword %s\n",
                             __FILENAME__, __LINE__, text);
                    return -1;
                }

                high = mid;
                continue;
            }
        }
    }

    logdebug("[%s, %d] Returning -1 for keyword %s\n",
             __FILENAME__, __LINE__, text);
    return -1;
}


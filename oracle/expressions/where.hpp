// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * where.hpp
 *
 * wrapper for logical_expression to create a where clause.
 *
 * Amrith - 2022-10-07
 */

#ifndef __WHERE_HPP_DEFINED__
#define __WHERE_HPP_DEFINED__

#include <aws/dynamodb/model/AttributeValue.h>
#include "logical_expression.hpp"
#include "logging.hpp"
#include "symbol_table.hpp"

namespace ddbsh
{
    class CWhere
    {
    public:
        CWhere(CLogicalExpression * wc) {
            // many things assume that if a method in this class is
            // executed, m_where is not NULL.
            assert(wc);
            m_where = wc;
        };

        ~CWhere() {
            logdebug("[%s, %d] In destructor\n", __FILENAME__, __LINE__);
            delete m_where;
            m_where = NULL;
        };

        // what follows are a collection of can_ methods. Each method will
        // return a boolean indicating whether or not the PK and RK
        // provided can be used to perform a specific operation. For
        // example, can_getitem() will return true if the where clause
        // will permit a getitem request.
        bool can_getitem(std::string pk, std::string rk) {
            // pk must always be specified.
            assert(!pk.empty());

            // getitem requires the complete primary key, and only the
            // primary key in the where clause.
            if (!rk.empty()) {
                if (m_where->count() == 2 && m_where->count_gud_key(pk) == 1 && m_where->count_gud_key(rk) == 1)
                    return true;
            } else {
                if (m_where->count() == 1 && m_where->count_gud_key(pk) == 1)
                    return true;
            }

            return false;
        };

        bool can_query_table(std::string pk, std::string rk) {
            // pk must always be specified.
            assert(!pk.empty());

            // doing a query on a table first requires that you can't
            // do a getitem().
            assert(!can_getitem(pk, rk));

            // for a query, you should at least have a pk check that
            // is an exact match.
            if (!(m_where->count_gud_key(pk) == 1 && m_where->count(pk) == 1))
                return false;

            return m_where->query_safe(pk, rk);
        };

        bool can_query_index(std::string pk, std::string rk) {
            // you cannot perform a getitem on an index since the key is
            // not unique. So can_query_index is can_getitem ||
            // can_query_table
            return (can_getitem(pk, rk) || can_query_table(pk, rk));
        };

        bool can_scan_table(std::string pk, std::string rk) {
            assert(!can_getitem(pk, rk) && !can_query_table(pk, rk));

            return true;
        };

        bool can_scan_index(std::string pk, std::string rk) {
            assert(!can_query_index(pk, rk));

            return true;
        };

        // can_update_item tells whether or not there is enough
        // information to uniquely identify an item. i.e. does it have a
        // full primary key.
        bool can_update_item(std::string pk, std::string rk) {
            // to update an item you must have a full primary key
            // specification. Anything else referencing the pk or rk, and
            // anything else can remain as a condition check expression.
            if (!rk.empty()) {
                if (m_where->count_gud_key(pk) == 1 && m_where->count_gud_key(rk) == 1)
                    return true;
            } else {
                if (m_where->count_gud_key(pk) == 1)
                    return true;
            }

            return false;
        };

        bool can_delete_item(std::string pk, std::string rk) {
            // this is identical to can_update_item
            return can_update_item(pk, rk);
        };

        // for any singleton get/update or delete, retreive the key
        Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> get_gud_key(std::string pk, std::string rk) {
            return m_where->get_gud_key(pk, rk);
        };

        // if we can query, fetch the key and filter
        Aws::String query_key_condition_expression(std::string pk, std::string rk, CSymbolTable * st) {
            return m_where->query_key_condition_expression(pk, rk, st);
        };

        Aws::String query_filter_expression(std::string pk, std::string rk, CSymbolTable * st) {
            return m_where->query_filter_expression(pk, rk, st);
        };

        // get the filter for a scan
        Aws::String scan_filter_expression(CSymbolTable * st) {
            return m_where->serialize(st);
        };

        Aws::String update_delete_condition_check(std::string pk, std::string rk, CSymbolTable * st) {
            return m_where->update_delete_condition_check(pk, rk, st, true /* top-level */);
        };

        std::string serialize(CSymbolTable * st) {
            // this is a convenience method and should never expect to
            // have a symbol table.
            assert(!st);
            return m_where->serialize(st);
        };

    private:
        CLogicalExpression * m_where;
    };
};
#endif

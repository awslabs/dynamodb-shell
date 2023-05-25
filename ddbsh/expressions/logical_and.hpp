// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * logical_and.hpp
 *
 * The logical and operator.
 *
 * Amrith - 2022-09-18
 */

#ifndef __LOGICAL_AND_HPP_DEFINED__
#define __LOGICAL_AND_HPP_DEFINED__

#include <vector>
#include <assert.h>
#include "logical_expression.hpp"
#include "allocation.hpp"

namespace ddbsh
{
    class CLogicalAnd : public CLogicalExpression
    {
    public:
        CLogicalAnd(CLogicalExpression * expr1, CLogicalExpression * expr2) {
            m_exprlist.push_back(expr1);
            m_exprlist.push_back(expr2);
            m_node_type = "CLogicalAnd";
        };

        CLogicalAnd(CLogicalExpression * expr1) {
            m_exprlist.push_back(expr1);
            m_node_type = "CLogicalAnd";
        };

        static CLogicalAnd * logical_and(CLogicalExpression * expr1, CLogicalExpression * expr2) {
            if (expr1->is_negated() == expr2->is_negated() &&
                expr1->type() == "CLogicalAnd" &&
                expr2->type() == "CLogicalAnd" &&
                !expr1->is_paren_group() &&
                !expr2->is_paren_group()) {

                CLogicalAnd * e1 = (CLogicalAnd *) expr1;
                CLogicalAnd * e2 = (CLogicalAnd *) expr2;

                e1->m_exprlist.insert(std::end(e1->m_exprlist), std::begin(e2->m_exprlist), std::end(e2->m_exprlist));

                delete e2;
                return e1;
            } else if (!expr1->is_negated() &&
                       expr1->type() == "CLogicalAnd") {
                CLogicalAnd * e1 = (CLogicalAnd *) expr1;
                e1->m_exprlist.push_back(expr2);
                return e1;
            } else {
                CLogicalAnd * rv = NEW CLogicalAnd (expr1, expr2);
                return rv;
            }
        };

        ~CLogicalAnd() {
            logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);
            for (int ix = 0; ix < m_exprlist.size(); ix ++)
            {
                CLogicalExpression * p = m_exprlist[ix];

                delete p;
            }
        };

        virtual int count(std::string lhs) {
            int ct = 0;
            for (auto t: m_exprlist) {
                ct += t->count(lhs);
            }

            logdebug("[%s, %d] count(%s) returns %d\n", __FILENAME__, __LINE__, lhs.c_str(), ct);
            return ct;
        };

        virtual int count() {
            int ct = 0;
            for (auto t: m_exprlist) {
                ct += t->count();
            }

            logdebug("[%s, %d] count() returns %d\n", __FILENAME__, __LINE__, ct);
            return ct;
        };

        virtual int count_gud_key(std::string lhs) {
            int ct = 0;
            for (auto t: m_exprlist) {
                ct += t->count_gud_key(lhs);
            }

            logdebug("[%s, %d] count_gud_key(%s) returns %d\n", __FILENAME__, __LINE__, lhs.c_str(), ct);
            return ct;
        };

        virtual int count_query_rk(std::string lhs) {
            int ct = 0;
            for (auto t: m_exprlist) {
                ct += t->count_query_rk(lhs);
            }

            logdebug("[%s, %d] count_gud_key(%s) returns %d\n", __FILENAME__, __LINE__, lhs.c_str(), ct);
            return ct;
        };

        virtual Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> get_gud_key(std::string pk, std::string rk) {
            Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> rv;

            for (auto t: m_exprlist) {
                Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> thisval;

                thisval = t->get_gud_key(pk, rk);
                if (thisval.size() > 0)
                    rv.insert(thisval.begin(), thisval.end());
            }

            logdebug("[%s, %d] get_gud_key() returns %d items\n", __FILENAME__, __LINE__, rv.size());
            return rv;
        };

    private:

        std::vector<CLogicalExpression *> m_exprlist;

        std::string __serialize(CSymbolTable * st) {
            std::string rv;
            bool first = true;

            if (is_paren_group()) {
                rv = "(";
            }

            for (auto p : m_exprlist) {
                if (!first)
                    rv = rv + " AND ";
                else
                    first = false;

                rv = rv + p->serialize(st);
            }

            if (is_paren_group()) {
                rv += ")";
            }

            return rv;
        };

        Aws::String __query_key_condition_expression(std::string pk, std::string rk, CSymbolTable * st) {
            std::string rv;
            bool first = true;

            for (auto p : m_exprlist) {
                std::string thisval = p->query_key_condition_expression(pk, rk, st);

                if (!thisval.empty()) {
                    if (!first)
                        rv = rv + " AND ";
                    else
                        first = false;

                    rv = rv + thisval;
                }
            }

            if (is_paren_group())
                rv = "(" + rv + ")";

            return rv;
        };

        Aws::String __query_filter_expression(std::string pk, std::string rk, CSymbolTable * st) {
            std::string rv;
            bool first = true;

            for (auto p : m_exprlist) {
                std::string thisval = p->query_filter_expression(pk, rk, st);

                if (!thisval.empty()) {
                    if (!first)
                        rv = rv + " AND ";
                    else
                        first = false;

                    rv = rv + thisval;
                }
            }

            if (is_paren_group())
                rv = "(" + rv + ")";

            return rv;
        };

        virtual Aws::String __update_delete_condition_check(std::string pk, std::string rk, CSymbolTable * st, bool top) {
            // this is an and, may be the top level and, but anything below this is also is top level.
            std::string rv;
            bool first = true;

            for (auto p : m_exprlist) {
                std::string thisval = p->update_delete_condition_check(pk, rk, st, top);

                if (!thisval.empty()) {
                    if (!first)
                        rv = rv + " AND ";
                    else
                        first = false;

                    rv = rv + thisval;
                }
            }

            if (is_paren_group())
                rv = "(" + rv + ")";

            return rv;
        };

        // run query_safe across all items that are and'ed
        // together. Fail fast.
        virtual bool __query_safe(std::string pk, std::string rk) {
            for (auto p: m_exprlist) {
                if (!p->query_safe(pk, rk))
                    return false;
            }

            return true;
        };
    };
};
#endif

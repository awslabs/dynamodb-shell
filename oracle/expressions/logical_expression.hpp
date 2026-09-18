// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * logical_expression.hpp
 *
 * The base class for a logical expression.
 *
 * Amrith - 2022-09-16
 */

#ifndef __LOGICAL_EXPRESSION_HPP_DEFINED__
#define __LOGICAL_EXPRESSION_HPP_DEFINED__

#include <assert.h>
#include "symbol_table.hpp"

namespace ddbsh
{
    class CLogicalExpression
    {
    public:
        CLogicalExpression() {
            m_negate = false;
            m_parens = false;
        };

        virtual ~CLogicalExpression() {
            logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);
        };

        // provide a string representation of the expression. Make no
        // changes to the expression - serialize can be called multiple
        // times.
        std::string serialize(CSymbolTable * st) {
            return m_negate ? "NOT (" + __serialize(st) + ")" : __serialize(st);
        };

        // is this expression negated
        virtual bool is_negated() final {
            return m_negate;
        };

        // setter for negate (which is a toggle)
        virtual void negate() final {
            bool old = m_negate;
            m_negate = m_negate ? false : true;
            logdebug("[%s, %d] toggled m_negate from %s to %s\n",
                     __FILENAME__, __LINE__, old ? "true" : "false", m_negate ? "true" : "false");
        };

        // a grouping of elements in a logical expression, a setter and
        // getter. these cannot be overridden.
        virtual void set_paren_group() final {
            m_parens = true;
        };

        virtual bool is_paren_group() final {
            return m_parens;
        };

        // counts nodes that are not conjunctions. This will have to be
        // overridden in conjunctions to recurse.
        virtual int count() {
            return 1;
        };

        virtual int count(std::string lhs) = 0;

        // does this node contain a check that would count towards a PK or
        // RK check for GUD? Default is 0, and is overridden in
        // CLogicalComparison and CLogicalAnd only.
        virtual int count_gud_key(std::string lhs) {
            return 0;
        };

        // fetch the key for gud. Default is to return an empty
        // map. CLogicalComparison and CLogicalAnd will override.
        virtual Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> get_gud_key(std::string pk, std::string rk) {
            Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> empty;
            return empty;
        };

        // does this node contain a check that counts towards a scan
        // (which must be on the RK element)? Overridden in a number of
        // places including CLogicalAnd.
        virtual int count_query_rk(std::string lhs) {
            return 0;
        };

        virtual std::string type() final {
            // typeof doesn't do what we need since nodes are stored in a
            // vector (in CLogicalAnd, and CLogicalOr). Therefore an
            // explicit type method is required, and it'll return the
            // actual node type.

            // however, CLogicalExpression can't have a type, so assert
            // instead. A class derived from CLogicalExpression will
            // initialize m_node_type in its constructor.
            assert(!m_node_type.empty());

            return m_node_type;
        };

        Aws::String query_key_condition_expression(std::string pk, std::string rk, CSymbolTable * st) {
            std::string rv = __query_key_condition_expression(pk, rk, st);

            if (!rv.empty()) {
                return m_negate ? "NOT (" + rv + ")" : rv;
            }

            return rv;
        };

        Aws::String query_filter_expression(std::string pk, std::string rk, CSymbolTable * st) {
            std::string rv = __query_filter_expression(pk, rk, st);

            if (!rv.empty()) {
                return m_negate ? "NOT (" + rv + ")" : rv;
            }

            return rv;
        };

        Aws::String update_delete_condition_check(std::string pk, std::string rk, CSymbolTable * st, bool top) {
            std::string rv = __update_delete_condition_check(pk, rk, st, top);

            if (!rv.empty()) {
                return m_negate ? "NOT (" + rv + ")" : rv;
            }

            return rv;
        };

        // query_safe looks over the entire where clause and
        // determines whether all clauses that it finds are ok in a
        // query. This basically amounts to the following.
        //
        // 1. Every derived class must implement query_safe, and most
        // of them will return true if they don't relate to the pk and
        // rk.
        //
        // 2. logical_and and logical_or will descend the tree.
        //
        // 3. logical_comparison equality is OK with pk if not
        // negated.
        //
        // 4. logical_comparison with rk is OK if it is not negated.
        //
        // 5. logical_between is OK with rk if it is not negated.
        //
        // 6. logical_begins_with is OK with RK if it is not negated.
        //
        virtual bool query_safe(std::string pk, std::string rk) {
            // wraps __query_safe();
            return __query_safe(pk, rk);
        };

    protected:
        bool m_negate;
        bool m_parens;
        std::string m_node_type;

    private:
        // make no change to the logical_expression (derived instance) -
        // it can be serialized multiple times (with a different symbol
        // table if required). If a symbol table is provided, populate it
        // and return a string with the tokens. If no symbol table is
        // provided use the values.
        virtual std::string __serialize(CSymbolTable * st) = 0;

        virtual Aws::String __query_key_condition_expression(std::string pk, std::string rk, CSymbolTable * st) = 0;
        virtual Aws::String __query_filter_expression(std::string pk, std::string rk, CSymbolTable * st) = 0;
        virtual Aws::String __update_delete_condition_check(std::string pk, std::string rk, CSymbolTable * st, bool top) = 0;
        virtual bool __query_safe(std::string pk, std::string rk) = 0;
    };
};
#endif

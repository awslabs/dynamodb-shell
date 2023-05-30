// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * logical_comparison.hpp
 *
 * The logical comparison operator.
 *
 * Amrith - 2022-09-18
 */

#ifndef __LOGICAL_COMPARISON_HPP_DEFINED__
#define __LOGICAL_COMPARISON_HPP_DEFINED__

#include <regex>
#include <aws/dynamodb/model/AttributeValue.h>
#include "serialize_attribute.hpp"

namespace ddbsh
{
    class CLogicalComparison : public CLogicalExpression
    {
    public:
        CLogicalComparison(std::string * lhs, char * op, Aws::DynamoDB::Model::AttributeValue * rhs) {
            m_lhs = *lhs;
            m_rhs = *rhs;
            m_node_type = "CLogicalComparison";

            if (!strcmp(op, "!="))
            {
                m_op = "=";
                this->negate();
            }
            else
            {
                m_op = op;
            }
        };

        int count(std::string lhs) {
            return m_lhs == lhs ? 1 : 0;
        };

        int count_gud_key(std::string lhs) {
            return m_lhs == lhs && !is_negated() && m_op == "=" ? 1 : 0;
        };

        int count_query_rk(std::string lhs) {
            logdebug("[%s, %d] m_op = %s\n", __FILENAME__, __LINE__, m_op.c_str());
            return m_lhs == lhs && !is_negated();
        };

        virtual Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> get_gud_key(std::string pk, std::string rk) {
            Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> rv;
            if (count_gud_key(pk) || count_gud_key(rk))
                rv.insert({m_lhs, m_rhs});

            return rv;
        };

        ~CLogicalComparison() {
            logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);
        };

    private:
        std::string __serialize(CSymbolTable * st) {
            std::string lhs = st ? st->new_attribute(this->m_lhs) : this->m_lhs;
            std::string rhs = st ? st->new_value(m_rhs) : serialize_attribute(m_rhs);
            std::string rv = lhs + " " + m_op + " " + rhs;
            return rv;
        };

        // this expression has no idea where it is in the tree. If it is
        // in a top level anded list, or the entire where clause, it would
        // show up as a key condition expression. But it may be somewhere
        // else in the where tree (under some OR for example). IN that
        // case it would only appear in the filter expression. So the
        // filter version always emits something.
        Aws::String __query_key_condition_expression(std::string pk, std::string rk, CSymbolTable * st) {
            assert(this->__query_safe(pk, rk));
            if (m_lhs == pk || m_lhs == rk)
                return this->__serialize(st);
            else
                return "";
        };

        Aws::String __query_filter_expression(std::string pk, std::string rk, CSymbolTable * st) {
            assert(this->__query_safe(pk, rk));
            if (!(pk == m_lhs || rk == m_lhs))
                return this->__serialize(st);
            else
                return "";
        };

        Aws::String __update_delete_condition_check(std::string pk, std::string rk, CSymbolTable * st, bool top) {
            if (!top)
                return this->__serialize(st);
            else
            {
                // if this is a key condition, no need to send it again.
                if ((m_lhs == pk || m_lhs == rk) && !is_negated())
                    return "";
                else
                    return this->__serialize(st);
            }
        };

        virtual bool __query_safe(std::string pk, std::string rk) {
            if (m_lhs != pk && m_lhs != rk)
                return true;

            // m_lhs is either pk or rk
            if (m_lhs == pk)
            {
                if (m_op == "=" && !is_negated())
                    return true;

                return false;
            }
            else if (m_lhs == rk) // this must be the case, but just to be sure.
            {
                if (m_op == "=" && is_negated())
                    return false;
            }

            return true;
        };

        std::string m_lhs;
        Aws::DynamoDB::Model::AttributeValue m_rhs;
        std::string m_op;
    };
};
#endif

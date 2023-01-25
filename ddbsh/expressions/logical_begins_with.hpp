// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * begins_with.hpp
 *
 * The logical begins_with operator.
 *
 * Amrith - 2022-09-18
 */

#ifndef __LOGICAL_BEGINS_WITH_HPP_DEFINED__
#define __LOGICAL_BEGINS_WITH_HPP_DEFINED__

#include <aws/dynamodb/model/AttributeValue.h>
#include "serialize_attribute.hpp"

namespace ddbsh
{
    class CLogicalBeginsWith : public CLogicalExpression
    {

    public:
        CLogicalBeginsWith(std::string *lhs, Aws::DynamoDB::Model::AttributeValue prefix) {
            m_lhs = *lhs;
            m_prefix = prefix;
            m_node_type = "CLogicalBeginsWith";
        };

        int count(std::string lhs) {
            return m_lhs == lhs ? 1 : 0;
        };

        int count_query_rk(std::string lhs) {
            return is_negated() ? 0 : count(lhs);
        };

        ~CLogicalBeginsWith() {
            logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);
        };

    private:
        std::string m_lhs;
        Aws::DynamoDB::Model::AttributeValue m_prefix;

        std::string __serialize(CSymbolTable * st) {
            std::string lhs = st ? st->new_attribute(m_lhs) : m_lhs;
            std::string rv = "begins_with(" + lhs + ", " +
                (st ? st->new_value(m_prefix) : ("\"" +  serialize_attribute(m_prefix) + "\"")) + ")";
            return rv;
        };

        Aws::String __query_key_condition_expression(std::string pk, std::string rk, CSymbolTable * st) {
            if ((m_lhs == pk || m_lhs == rk) && !is_negated())
                return this->__serialize(st);
            else
                return "";
        };

        Aws::String __query_filter_expression(std::string pk, std::string rk, CSymbolTable * st) {
            if (rk != m_lhs)
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
    };
};
#endif

// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * between.hpp
 *
 * The logical between operator.
 *
 * Amrith - 2022-09-18
 */

#ifndef __LOGICAL_BETWEEN_HPP_DEFINED__
#define __LOGICAL_BETWEEN_HPP_DEFINED__

#include <aws/dynamodb/model/AttributeValue.h>
#include "serialize_attribute.hpp"

namespace ddbsh
{
    class CLogicalBetween : public CLogicalExpression
    {

    public:
        CLogicalBetween(std::string * lhs, Aws::DynamoDB::Model::AttributeValue * l1, Aws::DynamoDB::Model::AttributeValue * l2) {
            m_lhs =*lhs;
            m_l1 = *l1;
            m_l2 = *l2;
            m_node_type = "CLogicalBetween";
        };

        int count(std::string lhs) {
            return m_lhs == lhs ? 1 : 0;
        };

        int count_query_rk(std::string lhs) {
            return is_negated() ? 0 : count(lhs);
        };

        ~CLogicalBetween() {
            logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);
        };

    private:
        Aws::DynamoDB::Model::AttributeValue m_l1;
        Aws::DynamoDB::Model::AttributeValue m_l2;
        std::string m_lhs;

        std::string __serialize(CSymbolTable * st) {
            std::string lhs = st ? st->new_attribute(m_lhs) : m_lhs;
            std::string r1 = st ? st->new_value(m_l1) : serialize_attribute(m_l1);
            std::string r2 = st ? st->new_value(m_l2) : serialize_attribute(m_l2);
            return lhs + " BETWEEN " + r1 + " AND " + r2;
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

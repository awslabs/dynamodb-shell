// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * logical_in.hpp
 *
 * The logical in operator.
 *
 * Amrith - 2022-09-18
 */

#ifndef __LOGICAL_IN_HPP_DEFINED__
#define __LOGICAL_IN_HPP_DEFINED__

#include <aws/dynamodb/model/AttributeValue.h>
#include "serialize_attribute.hpp"
#include "symbol_table.hpp"

namespace ddbsh
{
    class CLogicalIn : public CLogicalExpression
    {

    public:
        CLogicalIn(std::string *lhs, Aws::Vector<Aws::DynamoDB::Model::AttributeValue> *rhs) {
            m_lhs = *lhs;
            m_rhs = *rhs;
            m_node_type = "CLogicalIn";
        };

        int count(std::string lhs) {
            return m_lhs == lhs ? 1 : 0;
        };

        ~CLogicalIn() {
            logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);
        };

    private:

        std::string m_lhs;
        Aws::Vector<Aws::DynamoDB::Model::AttributeValue> m_rhs;

        std::string __serialize(CSymbolTable * st) {
            std::string rv = (st ? st->new_attribute(m_lhs) : m_lhs) + " IN (";
            bool first = true;

            for (auto f : m_rhs)
            {
                if (!first)
                {
                    rv += ", " + (st ? st->new_value(f) : serialize_attribute(f));
                    continue;
                }

                first = false;
                rv += st ? st->new_value(f) : serialize_attribute(f);
            }

            rv += ")";

            return rv;
        };

        Aws::String __query_key_condition_expression(std::string pk, std::string rk, CSymbolTable * st) {
            return "";
        };

        Aws::String __query_filter_expression(std::string pk, std::string rk, CSymbolTable * st) {
            if (m_lhs != rk)
                return this->__serialize(st);
            else
                return "";
        };

        Aws::String __update_delete_condition_check(std::string pk, std::string rk, CSymbolTable * st, bool top) {
            return this->__serialize(st);
        };
    };
};
#endif

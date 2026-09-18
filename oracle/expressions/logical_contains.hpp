// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * contains.hpp
 *
 * The logical contains operator.
 *
 * Amrith - 2022-09-18
 */

#ifndef __LOGICAL_CONTAINS_HPP_DEFINED__
#define __LOGICAL_CONTAINS_HPP_DEFINED__

namespace ddbsh
{
    class CLogicalContains : public CLogicalExpression
    {

    public:
        CLogicalContains(std::string *lhs, Aws::DynamoDB::Model::AttributeValue prefix) {
            m_lhs = *lhs;
            m_prefix = prefix;
            m_node_type = "CLogicalContains";
        };

        int count(std::string lhs) {
            return m_lhs == lhs ? 1 : 0;
        };

        ~CLogicalContains() {
            logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);
        };

    private:
        std::string m_lhs;
        Aws::DynamoDB::Model::AttributeValue m_prefix;

        std::string __serialize(CSymbolTable * st) {
            std::string lhs = st ? st->new_attribute(m_lhs) : m_lhs;
            std::string rv = "contains(" + lhs + ", " +
                (st ? st->new_value(m_prefix) : ("\"" + serialize_attribute(m_prefix) + "\"")) + ")";
            return rv;
        };

        Aws::String __query_key_condition_expression(std::string pk, std::string rk, CSymbolTable * st) {
            assert(this->__query_safe(pk, rk));
            return "";
        };

        Aws::String __query_filter_expression(std::string pk, std::string rk, CSymbolTable * st) {
            assert(this->__query_safe(pk, rk));
            return this->__serialize(st);
        };

        Aws::String __update_delete_condition_check(std::string pk, std::string rk, CSymbolTable * st, bool top) {
            return this->__serialize(st);
        };

        virtual bool __query_safe(std::string pk, std::string rk) {
            if (m_lhs != pk && m_lhs != rk)
                return true;

            return false;
        };
    };
};
#endif

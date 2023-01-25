// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * logical_attribute_exists.hpp
 *
 * The logical attribute existance operator.
 *
 * Amrith - 2022-09-18
 */

#ifndef __LOGICAL_ATTRIBUTE_EXISTS_HPP_DEFINED__
#define __LOGICAL_ATTRIBUTE_EXISTS_HPP_DEFINED__

namespace ddbsh
{
    class CLogicalAttributeExists : public CLogicalExpression
    {
    public:
        CLogicalAttributeExists(std::string *lhs, bool sense) {
            m_lhs = *lhs;
            // when parsing an ATTRIBUTE_EXISTS() sense will be true and
            // when parsing an ATTRIBUTE_NOT_EXISTS() sense will be
            // false. So the negate flag is !sense.
            m_negate = !sense;
            m_node_type = "CLogicalAttributeExists";
        };

        int count(std::string lhs) {
            return m_lhs == lhs ? 1 : 0;
        };

        ~CLogicalAttributeExists() {
            logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);
        };

    private:
        std::string m_lhs;
        std::string __serialize(CSymbolTable * st) {
            std::string lhs = st ? st->new_attribute(m_lhs) : m_lhs;
            std::string rv =  "attribute_exists(" + lhs + ")";
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

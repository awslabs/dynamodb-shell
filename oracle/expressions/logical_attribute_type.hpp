// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * logical_attribute_type.hpp
 *
 * The logical attribute type operator.
 *
 * Amrith - 2022-09-18
 */

#ifndef __LOGICAL_ATTRIBUTE_TYPE_HPP_DEFINED__
#define __LOGICAL_ATTRIBUTE_TYPE_HPP_DEFINED__

#include <aws/dynamodb/model/ScalarAttributeType.h>

namespace ddbsh
{
    class CLogicalAttributeType : public CLogicalExpression
    {
    public:
        CLogicalAttributeType(std::string * lhs, Aws::DynamoDB::Model::ScalarAttributeType attr_type) {
            m_lhs = *lhs;
            m_type = attr_type;
            m_node_type = "CLogicalAttributeType";
        };

        int count(std::string lhs) {
            return m_lhs == lhs ? 1 : 0;
        };

        ~CLogicalAttributeType() {
            logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);
        };

    private:
        std::string m_lhs;
        Aws::DynamoDB::Model::ScalarAttributeType m_type;

        std::string __serialize(CSymbolTable * st) {
            std::string lhs = st ? st->new_attribute(m_lhs) : m_lhs;
            std::string attype;

            if (st)
            {
                Aws::DynamoDB::Model::AttributeValue av;
                av.SetS(Aws::DynamoDB::Model::ScalarAttributeTypeMapper::GetNameForScalarAttributeType(m_type));
                attype = st->new_value(av);
            }
            else
                attype = Aws::DynamoDB::Model::ScalarAttributeTypeMapper::GetNameForScalarAttributeType(m_type);

            return "attribute_type(" + lhs + ", " + attype + ")";
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

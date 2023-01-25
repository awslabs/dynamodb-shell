// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * update_set_element.hpp
 *
 * The update set element
 *
 * Amrith - 2022-11-06
 */

#ifndef __UPDATE_SET_ELEMENT_HPP_DEFINED__
#define __UPDATE_SET_ELEMENT_HPP_DEFINED__

#include "aws-includes.hpp"
#include "symbol_table.hpp"
#include "serialize_attribute.hpp"
#include "strip_newlines.hpp"

namespace ddbsh
{
    class CUpdateSetElement
    {
    public:
        CUpdateSetElement() {
            m_rhs_value_is_set = false;
        };

        void set_lhs(Aws::String * lhs) {
            m_lhs = *lhs;
        };

        void set_use_lhs(Aws::String lhs) {
            m_rhs_attribute = lhs;
        };

        void set_if_not_exists(Aws::String * lhs, Aws::DynamoDB::Model::AttributeValue * d) {
            m_if_not_exists_lhs = * lhs;
            m_if_not_exists_value = * d;
        };

        void set_rhs_value(Aws::DynamoDB::Model::AttributeValue * rhs) {
            m_rhs_value_is_set = true;
            m_rhs_value = * rhs;
        };

        void set_op(Aws::String op) {
            m_op = op;
        };

        ~CUpdateSetElement() {
        };

        Aws::String serialize(CSymbolTable * st) {
            return __serialize(st);
        };

    private:
        Aws::String __serialize(CSymbolTable * st) {
            Aws::String rv;

            assert(!m_lhs.empty());

            rv = (st ? st->new_attribute(m_lhs) : m_lhs) + " = ";

            if (!m_rhs_attribute.empty())
            {
                rv += (st ? st->new_attribute(m_rhs_attribute) : m_rhs_attribute);
            }
            else if (!m_if_not_exists_lhs.empty())
            {
                rv += "if_not_exists(" + (st ? st->new_attribute(m_if_not_exists_lhs) : m_if_not_exists_lhs) + ",";

                rv += (st ? st->new_value(m_if_not_exists_value) : serialize_attribute(m_if_not_exists_value));
                rv += ")";
            }

            if (!m_op.empty())
                rv +=  m_op;

            if (m_rhs_value_is_set)
                rv += (st ? st->new_value(m_rhs_value) : serialize_attribute(m_rhs_value));

            logdebug("[%s, %d] returning %s\n", __FILENAME__, __LINE__, rv.c_str());
            return rv;
        };

        Aws::String m_lhs;
        Aws::String m_op;

        bool m_rhs_value_is_set;
        Aws::DynamoDB::Model::AttributeValue m_rhs_value;

        Aws::String m_rhs_attribute;
        Aws::String m_if_not_exists_lhs;
        Aws::DynamoDB::Model::AttributeValue m_if_not_exists_value;
    };
};
#endif

// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * symbol_table.hpp
 *
 * Provides a mapping of attribute names and values
 *
 * Amrith - 2022-10-30
 *
 */

#ifndef __SYMBOL_TABLE_HPP_DEFINED__
#define __SYMBOL_TABLE_HPP_DEFINED__

#include "aws-includes.hpp"
#include "logging.hpp"
#include "serialize_attribute.hpp"
#include "ddbsh.hpp"

namespace ddbsh
{
    class CSymbolTable
    {
    public:
        CSymbolTable() {
            prefix = CDDBSh::getInstance()->st_unique_prefix();
            m_attr_ix = 1;
            m_value_ix = 1;
        };

        // given a name, add it to the list of names with a unique ix if
        // it does not already exist.
        //
        // new_attribute is quite complex because it hides all the
        // magic around replacing names in the case of maps and lists.
        //
        // you can make the case that this is wrong. after all there
        // is a front end parser that constructs m_lhs and has the
        // piece parts - shouldn't it be part of this?
        //
        std::string new_attribute(std::string name) {
            std::string token;
            size_t endix, startix = 0;
            // look for map delimiters.

            do
            {
                std::string thistoken;
                endix = name.find((const char *) ".", startix);

                if (endix == std::string::npos)
                    thistoken = name.substr(startix, endix);
                else
                    thistoken = name.substr(startix, endix - startix);

                if (token.empty())
                    token = _new_attribute(thistoken);
                else
                    token = token + "." + _new_attribute(thistoken);

                startix = endix + 1;
            } while (endix != std::string::npos);

            return token;
        }

        // similarly, given a value add it to the list of values with a unique ix.
        std::string new_value(Aws::DynamoDB::Model::AttributeValue value) {
            std::string token = ":v" + this->prefix + std::to_string(m_value_ix++);
            m_values.emplace(token, value);

            logdebug("[%s, %d] Created token %s for value %s.\n", __FILENAME__, __LINE__,
                     token.c_str(), serialize_attribute(value).c_str());

            return token;
        };

        bool has_names() {
            return (m_names.size() > 0);
        };

        bool has_values() {
            return (m_values.size() > 0);
        };

        // the name map is keyed on the attribute name, not the
        // substitution. Swap it around before returning.
        Aws::Map<Aws::String, Aws::String> get_names() {
            Aws::Map<Aws::String, Aws::String> outmap;

            for (const auto &p : m_names) {
                outmap.emplace(p.second, p.first);
            }

            return outmap;
        };

        Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> get_values() {
            return m_values;
        };

    private:
        std::string prefix;
        int m_attr_ix;
        int m_value_ix;

        Aws::Map<Aws::String, Aws::String> m_names;
        Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> m_values;

        // _new_attribute receives a name that may contain a list
        // suffix. If it does, take the variable name, and only place
        // that in the symbol table and retain the subscript as is.
        std::string _new_attribute(std::string name) {
            size_t endix = name.find("[");
            if (endix != std::string::npos) {
                return __new_attribute(name.substr(0, endix)) + name.substr(endix);
            } else {
                return __new_attribute(name);
            }
        };

        // __new_attribute receives a name that has neither map
        // delimiter (.) or subscripts.
        std::string __new_attribute(std::string name) {
            int attr_ix = m_attr_ix;
            std::string token = "#a" + this->prefix + std::to_string(attr_ix);
            const auto newattr = m_names.emplace(name, token);

            if (newattr.second)
            {
                m_attr_ix ++;
                logdebug("[%s, %d] Created token %s for attribute %s\n", __FILENAME__, __LINE__, token.c_str(), name.c_str());
            }
            else
            {
                token = (*newattr.first).second;
                logdebug("[%s, %d] Reused token %s for attribute %s\n", __FILENAME__, __LINE__, token.c_str(), name.c_str());
            }

            return token;
        };

    };
};
#endif

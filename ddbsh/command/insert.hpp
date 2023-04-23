// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * insert.hpp
 *
 * the insert and replace commands
 *
 * Amrith - 2022-11-06
 */

#ifndef __INSERT_HPP_DEFINED__
#define __INSERT_HPP_DEFINED__

#include "aws-includes.hpp"
#include "parser-defs.hpp"
#include "logging.hpp"
#include "command.hpp"
#include "symbol_table.hpp"
#include "ratelimit.hpp"

namespace ddbsh
{
    class CInsertCommand : public CCommand
    {
    public:
        CInsertCommand(std::string table_name,
                       Aws::Vector<Aws::String> * column_list,
                       Aws::Vector<Aws::Vector<Aws::DynamoDB::Model::AttributeValue>> * values,
                       bool insert,
                       CRateLimit * ratelimit) {
            m_table_name = table_name;
            m_column_list = column_list;
            m_values = values;
            m_insert = insert;
            m_ratelimit = ratelimit;
        };

        ~CInsertCommand() {
            delete m_column_list;
            delete m_values;
            delete m_ratelimit;
        };

        virtual int run();

        virtual Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * txwrite();

    private:
        int make_put_request(Aws::DynamoDB::Model::PutItemRequest *, std::string, int);
        std::string m_table_name;
        Aws::Vector<Aws::String> * m_column_list;
        Aws::Vector<Aws::Vector<Aws::DynamoDB::Model::AttributeValue>> * m_values;
        bool m_insert;
        CRateLimit * m_ratelimit;
        bool valid();
    };
};
#endif


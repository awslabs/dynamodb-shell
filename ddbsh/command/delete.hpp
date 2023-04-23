// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * delete.hpp
 *
 * command handler for delete command
 *
 * Amrith - 2022-11-06
 */

#ifndef __DELETE_COMMAND_HPP_DEFINED__
#define __DELETE_COMMAND_HPP_DEFINED__

#include "command.hpp"
#include "where.hpp"
#include "update_delete.hpp"
#include "ratelimit.hpp"

namespace ddbsh
{
    class CDeleteCommand: public CUpdateDeleteCommand
    {
    public:
        CDeleteCommand(std::string table, CWhere * where, CRateLimit * ratelimit){
            logdebug("[%s, %d] In %s. setting m_delete to true.\n", __FILENAME__, __LINE__, __FUNCTION__);
            m_where = where;
            m_table_name = table;
            m_rate_limit = ratelimit;
        };

        int run();
        ~CDeleteCommand();

        virtual Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * txwrite();

    private:
        std::string m_table_name;
        CWhere * m_where;
        CRateLimit * m_rate_limit;

        int do_scan(std::string pk, std::string rk);
        int do_query(std::string pk, std::string rk);
        int do_deleteitem(std::string pk, std::string rk);

        Aws::DynamoDB::Model::DeleteItemRequest * make_delete_request(std::string pk, std::string rk, CSymbolTable * st);

        int do_delete(Aws::DynamoDB::Model::DeleteItemRequest * uir,
                      Aws::Map< Aws::String, Aws::DynamoDB::Model::AttributeValue> key);
    };
}
#endif

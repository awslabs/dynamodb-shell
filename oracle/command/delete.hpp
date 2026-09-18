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
        CDeleteCommand(Aws::Vector<Aws::String> * table, CWhere * where, CRateLimit * ratelimit,
                       Aws::DynamoDB::Model::ReturnValue returnvalue){
            logdebug("[%s, %d] In %s. setting m_delete to true.\n", __FILENAME__, __LINE__, __FUNCTION__);
            m_table_name = (*table)[0];
            if (table->size() > 1)
                m_index_name = (*table)[1];

            m_where = where;
            m_rate_limit = ratelimit;
            m_returnvalue = returnvalue;
        };

        int run();
        ~CDeleteCommand();

        virtual Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * txwrite();

    private:
        std::string m_table_name;
        std::string m_index_name;
        CWhere * m_where;
        CRateLimit * m_rate_limit;
        Aws::DynamoDB::Model::ReturnValue m_returnvalue;

        int do_scan(std::string pk, std::string rk);
        int do_query(std::string pk, std::string rk);
        int do_deleteitem(std::string pk, std::string rk);

        Aws::DynamoDB::Model::DeleteItemRequest * make_delete_request(std::string pk, std::string rk, CSymbolTable * st);

        int do_delete(Aws::DynamoDB::Model::DeleteItemRequest * uir,
                      Aws::Map< Aws::String, Aws::DynamoDB::Model::AttributeValue> key);
    };
}
#endif

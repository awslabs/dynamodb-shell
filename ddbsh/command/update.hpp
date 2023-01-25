// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * update.hpp
 *
 * command handler for update command
 *
 * Amrith - 2022-11-06
 */

#ifndef __UPDATE_COMMAND_HPP_DEFINED__
#define __UPDATE_COMMAND_HPP_DEFINED__

#include "command.hpp"
#include "update_delete.hpp"
#include "update_set_element.hpp"
#include "where.hpp"

namespace ddbsh
{
    class CUpdateCommand: public CUpdateDeleteCommand
    {
    public:
        CUpdateCommand(){
            logdebug("[%s, %d] In %s. setting m_update to true.\n", __FILENAME__, __LINE__, __FUNCTION__);
            m_update = true;
            m_set = NULL;
            m_remove = NULL;
            m_where = NULL;
        };

        int run();
        ~CUpdateCommand();

        void set(std::string table, Aws::Vector<CUpdateSetElement> * s, CWhere * where) {
            m_table_name = table;
            m_set = s;
            m_where = where;
        };

        void remove(std::string table, Aws::Vector<Aws::String> * r, CWhere * where) {
            m_table_name = table;
            m_remove = r;
            m_where = where;
        };

        void set_upsert() {
            logdebug("[%s, %d] setting upsert.\n", __FILENAME__, __LINE__);
            m_update = false;
        };

        virtual Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * txwrite();

    private:
        std::string m_table_name;
        CWhere * m_where;
        Aws::Vector<CUpdateSetElement> * m_set;
        Aws::Vector<Aws::String> * m_remove;
        bool m_update;

        int do_scan(std::string pk, std::string rk);
        int do_query(std::string pk, std::string rk);
        int do_updateitem(std::string pk, std::string rk);
        Aws::DynamoDB::Model::UpdateItemRequest * make_update_request(std::string pk, std::string rk, CSymbolTable * st);

        int do_update(Aws::DynamoDB::Model::UpdateItemRequest * uir,
                      Aws::Map< Aws::String, Aws::DynamoDB::Model::AttributeValue> key);


    };
}
#endif

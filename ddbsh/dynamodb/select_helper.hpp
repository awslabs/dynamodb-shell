// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * select_helper.hpp
 *
 * A simple helper that will build a request for either GetItem, Query, or Scan.
 *
 * Amrith - 2022-11-07
 */

#ifndef __SELECT_HELPER_HPP_DEFINED__
#define __SELECT_HELPER_HPP_DEFINED__

#include "aws-includes.hpp"
#include "where.hpp"

namespace ddbsh
{
    class CSelectHelper
    {
    public:
        CSelectHelper() {
            delete_where = false;
        };

        ~CSelectHelper() {
            logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);
            delete m_projection;
            if (delete_where)
                delete m_where;
        };

        bool can_getitem();
        bool can_query();

        std::string get_pk() {
            return m_pk;
        };

        std::string get_rk() {
            return m_rk;
        };

        // full blown request as needed by select
        int setup(bool consistent, Aws::Vector<Aws::String> * projection, Aws::Vector<Aws::String> * table, CWhere * where,
                  Aws::DynamoDB::Model::ReturnConsumedCapacity returns, bool ratelimit);

        Aws::DynamoDB::Model::GetItemRequest * getitem_request();
        Aws::DynamoDB::Model::QueryRequest * query_request();
        Aws::DynamoDB::Model::ScanRequest * scan_request();
        Aws::DynamoDB::Model::TransactGetItem * txget();
        Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * txwrite();

        // the quick and dirty, needed by update and delete

        int setup(std::string table, std::string index, CWhere * where, bool ratelimit);

        static Aws::Vector<std::string> show_items(const Aws::Vector<Aws::Map<Aws::String,
                                                   Aws::DynamoDB::Model::AttributeValue>>& items);
        static Aws::String show_item(const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> &item);

    private:
        std::string m_table_name;
        std::string m_index_name;
        std::string m_pk;
        std::string m_rk;
        bool delete_where;
        bool m_rate_limited;
        CWhere * m_where;
        bool m_consistent;
        Aws::Vector<Aws::String> * m_projection;
        Aws::DynamoDB::Model::ReturnConsumedCapacity m_consumed_capacity;

        std::string serialize_projection(CSymbolTable * st);
        std::string serialize_projection();
        static std::string quote_if_required(std::string input);
        static Aws::String serialize_value(Aws::DynamoDB::Model::AttributeValue v);
    };
};
#endif

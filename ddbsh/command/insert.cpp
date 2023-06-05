// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * insert.cpp
 *
 * the insert and replace commands
 *
 * Amrith - 2022-11-06
 */

#include "insert.hpp"
#include "ddbsh.hpp"
#include "logging.hpp"
#include "describe_helper.hpp"
#include "symbol_table.hpp"
#include "get_key_schema.hpp"
#include "strip_newlines.hpp"
#include "select_helper.hpp"

using namespace ddbsh;

int CInsertCommand::make_put_request(Aws::DynamoDB::Model::PutItemRequest * prequest, std::string pk, int iy)
{
    Aws::Map< Aws::String, Aws::DynamoDB::Model::AttributeValue> nvmap;

    for (int ix = 0; ix < m_column_list->size(); ix ++)
        nvmap.emplace((*m_column_list)[ix], (*m_values)[iy][ix]);

    prequest->SetTableName(m_table_name);
    prequest->SetItem(nvmap);

    if (m_returnvalue != Aws::DynamoDB::Model::ReturnValue::NONE)
        prequest->SetReturnValues(m_returnvalue);

    if (m_ratelimit)
        prequest->SetReturnConsumedCapacity(Aws::DynamoDB::Model::ReturnConsumedCapacity::TOTAL);

    if (m_insert)
    {
        CSymbolTable st;

        // for inserts add the one additional check on PK not exists
        std::string pkname = st.new_attribute(pk);

        std::string condition = "attribute_not_exists(" + pkname + ")";

        prequest->SetConditionExpression(condition);
        logdebug("[%s, %d] adding condition expression %s for insert\n", __FILENAME__, __LINE__, condition.c_str());
        prequest->SetExpressionAttributeNames(st.get_names());
    }

    return 0;
}

bool CInsertCommand::valid()
{
    int sz = m_column_list->size();

    for (int iy = 0; iy < m_values->size(); iy ++)
    {
        if (sz != ((*m_values)[iy]).size())
        {
            logerror("[%s, %d] Unequal number of column names (%d) and values (%d) in tuple %d\n",
                     __FILENAME__, __LINE__, sz, ((*m_values)[iy]).size(), iy + 1);
            return false;
        }
    }

    return true;
}

int CInsertCommand::run()
{
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
    p_dynamoDBClient = CDDBSh::getInstance()->getClient();
    const char * op = m_insert ? "INSERT" : "REPLACE";
    int rv;

    logdebug("[%s, %d] In %s\n", __FILENAME__, __LINE__, __FUNCTION__);
    logdebug("[%s, %d] table = %s\n", __FILENAME__, __LINE__, m_table_name.c_str());
    logdebug("[%s, %d] operation = %s\n", __FILENAME__, __LINE__, op);

    std::string pk, rk;

    if (get_key_schema(m_table_name, &pk, &rk) != 0)
        return -1;

    assert(!pk.empty());

    if (!valid())
        return -1;

    for (int iy = 0; iy < m_values->size(); iy ++)
    {
        Aws::DynamoDB::Model::PutItemRequest request;

        if ((rv = make_put_request(&request, pk, iy)) != 0)
            return rv;

        logdebug("[%s, %d] request = %s\n", __FILENAME__, __LINE__, strip_newlines(request.SerializePayload()).c_str());

        if (explaining())
        {
            printf("PutItem(%s)\n", explain_string(request.SerializePayload()).c_str());
        }
        else
        {
            const Aws::DynamoDB::Model::PutItemOutcome& result = p_dynamoDBClient->PutItem(request);

            if (result.IsSuccess())
            {
                logdebug("[%s, %d] %s succeeded.\n", __FILENAME__, __LINE__, op);
            }
            else
            {
                logerror("[%s, %d] %s failed. %s. %s. %s\n", __FILENAME__, __LINE__, op,
                         result.GetError().GetExceptionName().c_str(),
                         result.GetError().GetRequestId().c_str(),
                         result.GetError().GetMessage().c_str());
                return 1;
            }

            if (m_returnvalue !=  Aws::DynamoDB::Model::ReturnValue::NONE)
            {
                std::string i = CSelectHelper::show_item(result.GetResult().GetAttributes());
                printf("%s: %s\n", op, i.c_str());
            }
            else
            {
                printf("%s\n", op);
            }

            if (m_ratelimit)
            {
                m_ratelimit->consume_writes(result.GetResult().GetConsumedCapacity());
                m_ratelimit->writelimit();
            }
        }
    }

    return 0;
};

Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * CInsertCommand::txwrite()
{
    int retval;
    std::string pk, rk;

    if (get_key_schema(m_table_name, &pk, &rk) != 0)
        return NULL;

    assert(!pk.empty());

    if (!valid())
        return NULL;

    Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * rv = NEW Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem>;
    Aws::DynamoDB::Model::TransactWriteItem twi;
    Aws::DynamoDB::Model::Put put;

    for (int iy = 0; iy < m_values->size(); iy ++)
    {
        Aws::DynamoDB::Model::PutItemRequest request;

        if ((retval = make_put_request(&request, pk, iy)) != 0)
        {
            delete rv;
            return NULL;
        }

        put.SetItem(request.GetItem());
        put.SetTableName(request.GetTableName());

        if (request.GetConditionExpression().length())
            put.SetConditionExpression(request.GetConditionExpression());

        if (request.GetExpressionAttributeNames().size())
            put.SetExpressionAttributeNames(request.GetExpressionAttributeNames());

        if (request.GetExpressionAttributeValues().size())
            put.SetExpressionAttributeValues(request.GetExpressionAttributeValues());

        twi.SetPut(put);
        rv->push_back(twi);
    }

    return rv;
};

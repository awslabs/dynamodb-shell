// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * select.cpp
 *
 * The implementation of CSelectCommand
 *
 * Amrith - 2022-10-07
 */

#include "trim_double.hpp"
#include "select.hpp"
#include "ddbsh.hpp"
#include "logging.hpp"
#include "describe_helper.hpp"
#include "symbol_table.hpp"
#include "strip_newlines.hpp"
#include "consumed_capacity_totalizer.hpp"
#include "get_key_schema.hpp"
#include "select_helper.hpp"

using namespace ddbsh;

int CSelectCommand::run()
{
    if (m_helper.can_getitem())
    {
        logdebug("[%s, %d] will attempt do_getitem()\n", __FILENAME__, __LINE__);
        return do_getitem();
    }
    else if (m_helper.can_query())
    {
        logdebug("[%s, %d] will attempt do_query()\n", __FILENAME__, __LINE__);
        return do_query();
    }
    else
    {
        logdebug("[%s, %d] will attempt do_scan()\n", __FILENAME__, __LINE__);
        return do_scan();
    }
};

void CSelectCommand::display_consumed_capacity (const Aws::DynamoDB::Model::ConsumedCapacity cc)
{
    if (explaining())
        return;

    // consumed capacity could be set for ratelimiting, don't show it
    // if user didn't request it.
    if (m_returns == Aws::DynamoDB::Model::ReturnConsumedCapacity::NONE)
        return;

    if (cc.TableNameHasBeenSet())
        logdebug("[%s, %d] table name %s\n", __FILENAME__, __LINE__, cc.GetTableName().c_str());

    if (cc.CapacityUnitsHasBeenSet())
        logdebug("[%s, %d] capacity total: %f\n", __FILENAME__, __LINE__, cc.GetCapacityUnits());

    if (cc.ReadCapacityUnitsHasBeenSet())
        logdebug("[%s, %d] capacity rcu: %f\n", __FILENAME__, __LINE__, cc.GetReadCapacityUnits());

    if (cc.WriteCapacityUnitsHasBeenSet())
        logdebug("[%s, %d] capacity wcu: %f\n", __FILENAME__, __LINE__, cc.GetWriteCapacityUnits());

    if (cc.TableNameHasBeenSet() &&
        (cc.WriteCapacityUnitsHasBeenSet() || cc.ReadCapacityUnitsHasBeenSet() || cc.CapacityUnitsHasBeenSet()))
    {
        std::string capacity = cc.GetTableName();
        capacity += ", " + (cc.CapacityUnitsHasBeenSet() ? trim_double(std::to_string(cc.GetCapacityUnits())) : "0");
        capacity += ", " + (cc.ReadCapacityUnitsHasBeenSet() ? trim_double(std::to_string(cc.GetReadCapacityUnits())) : "0");
        capacity += ", " + (cc.WriteCapacityUnitsHasBeenSet() ? trim_double(std::to_string(cc.GetWriteCapacityUnits())) : "0");

        printf("SELECT (%s)\n", capacity.c_str());
    }
}

int CSelectCommand::do_getitem()
{
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
    p_dynamoDBClient = CDDBSh::getInstance()->getClient();
    Aws::DynamoDB::Model::GetItemRequest * request;

    logdebug("[%s, %d] Entering %s\n", __FILENAME__, __LINE__, __FUNCTION__);

    request = m_helper.getitem_request();

    if (explaining())
    {
        printf("GetItem(%s)\n", explain_string(request->SerializePayload()).c_str());
    }
    else
    {
        int ct = 0;
        const Aws::DynamoDB::Model::GetItemOutcome& result = p_dynamoDBClient->GetItem(*request);
        CConsumedCapacityTotalizer cc;

        if (result.IsSuccess())
        {
            const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> &item = result.GetResult().GetItem();

            if (!explaining())
            {
                std::string i = CSelectHelper::show_item(item);
                if (i.length())
                    printf("%s\n", i.c_str());
            }

            cc.add(result.GetResult().GetConsumedCapacity());
        }
        else
        {
            logerror("[%s, %d] SELECT failed. %s. %s. %s\n", __FILENAME__, __LINE__,
                     result.GetError().GetExceptionName().c_str(),
                     result.GetError().GetRequestId().c_str(),
                     result.GetError().GetMessage().c_str());
        }

        display_consumed_capacity(cc.get());
    }

    delete request;
    return 0;
}

int CSelectCommand::do_query()
{
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
    p_dynamoDBClient = CDDBSh::getInstance()->getClient();
    Aws::DynamoDB::Model::QueryRequest * request;

    logdebug("[%s, %d] Entering %s\n", __FILENAME__, __LINE__, __FUNCTION__);

    request = m_helper.query_request();

    int ct = 0, retval = 0;
    CConsumedCapacityTotalizer cc;

    do
    {
        if (explaining())
        {
            printf("Query(%s)\n", explain_string(request->SerializePayload()).c_str());
        }

        const Aws::DynamoDB::Model::QueryOutcome& result = p_dynamoDBClient->Query(*request);
        if (result.IsSuccess())
        {
            const Aws::Vector<Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue>> &items =
                result.GetResult().GetItems();

            if (!explaining())
            {
                for (auto f: CSelectHelper::show_items(items))
                {
                    printf("%s\n", f.c_str());
                }
            }

            cc.add(result.GetResult().GetConsumedCapacity());
        }
        else
        {
            logerror("[%s, %d] SELECT failed. %s. %s. %s\n", __FILENAME__, __LINE__,
                     result.GetError().GetExceptionName().c_str(),
                     result.GetError().GetRequestId().c_str(),
                     result.GetError().GetMessage().c_str());
            retval = -1;
            break;
        }

        if (result.GetResult().GetLastEvaluatedKey().empty())
            break;

        request->SetExclusiveStartKey(result.GetResult().GetLastEvaluatedKey());

        if (m_ratelimit)
        {
            m_ratelimit->consume_reads(result.GetResult().GetConsumedCapacity());
            m_ratelimit->readlimit();
        }
    } while(1);

    if (m_returns != Aws::DynamoDB::Model::ReturnConsumedCapacity::NONE)
        display_consumed_capacity(cc.get());

    delete request;
    return retval;
};

int CSelectCommand::do_scan()
{
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;
    p_dynamoDBClient = CDDBSh::getInstance()->getClient();
    Aws::DynamoDB::Model::ScanRequest * request;

    logdebug("[%s, %d] Entering %s\n", __FILENAME__, __LINE__, __FUNCTION__);

    request = m_helper.scan_request();

    int ct = 0, retval = 0;
    CConsumedCapacityTotalizer cc;

    do
    {
        if (explaining())
        {
            printf("Scan(%s)\n", explain_string(request->SerializePayload()).c_str());
        }

        const Aws::DynamoDB::Model::ScanOutcome& result = p_dynamoDBClient->Scan(*request);
        if (result.IsSuccess())
        {
            const Aws::Vector<Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue>> &items = result.GetResult().GetItems();

            if (!explaining())
            {
                for (auto f: CSelectHelper::show_items(items))
                {
                    printf("%s\n", f.c_str());
                }
            }

            cc.add(result.GetResult().GetConsumedCapacity());
        }
        else
        {
            logdebug("[%s, %d] SELECT failed. %s. %s. %s\n", __FILENAME__, __LINE__,
                     result.GetError().GetExceptionName().c_str(),
                     result.GetError().GetRequestId().c_str(),
                     result.GetError().GetMessage().c_str());
            printf("SELECT failed. %s. %s. %s\n",
                   result.GetError().GetExceptionName().c_str(),
                   result.GetError().GetRequestId().c_str(),
                   result.GetError().GetMessage().c_str());
            retval = -1;
            break;
        }

        if (result.GetResult().GetLastEvaluatedKey().empty())
            break;

        request->SetExclusiveStartKey(result.GetResult().GetLastEvaluatedKey());

        if (m_ratelimit)
        {
            m_ratelimit->consume_reads(result.GetResult().GetConsumedCapacity());
            m_ratelimit->readlimit();
        }
    } while(1);

    if (m_returns != Aws::DynamoDB::Model::ReturnConsumedCapacity::NONE)
        display_consumed_capacity(cc.get());

    delete request;
    return retval;
};

Aws::DynamoDB::Model::TransactGetItem * CSelectCommand::txget()
{
    return m_helper.txget();
};

Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * CSelectCommand::txwrite()
{
    if (m_exists)
        return m_helper.txwrite();
    else
    {
        // this should never happen as we can only get to a select in transact get items call.
        // so if we are here, we should log error.
        logdebug("[%s, %d] Unexpected call to txwrite without m_exists.\n", __FILENAME__, __LINE__);
        return NULL;
    }
};

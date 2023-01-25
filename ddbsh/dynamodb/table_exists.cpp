// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * table_exists.cpp
 *
 * helper to check if a table exists.
 *
 * Amrith - 2022-08-12
 */

#include "table_exists.hpp"
#include "logging.hpp"

#include "aws-includes.hpp"

bool table_exists (std::string name,
                   Aws::DynamoDB::DynamoDBClient * pClient)
{
    Aws::DynamoDB::Model::DescribeTableRequest dtr;
    dtr.SetTableName(name);
    const Aws::DynamoDB::Model::DescribeTableOutcome& result =  \
        pClient->DescribeTable(dtr);

    if (result.IsSuccess())
    {
        logdebug("[%s, %d] Found table %s to exist.\n", __FILENAME__, __LINE__,
                 name.c_str());
        Aws::DynamoDB::Model::DescribeTableResult dtresult = result.GetResult();
        Aws::DynamoDB::Model::TableDescription td = dtresult.GetTable();

        logdebug("[%s, %d] status = %s\n", __FILENAME__, __LINE__,
                 Aws::DynamoDB::Model::TableStatusMapper::GetNameForTableStatus(td.GetTableStatus()).c_str());
        return true;
    }
    else
    {
        if (!(result.GetError().GetErrorType() ==
              Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND))
            logerror("[%s, %d] %s. %s. %s\n", __FILENAME__, __LINE__,
                     result.GetError().GetExceptionName().c_str(),
                     result.GetError().GetRequestId().c_str(),
                     result.GetError().GetMessage().c_str());

        return false;
    }
}

bool table_active(std::string name, Aws::DynamoDB::DynamoDBClient * pClient)
{
    Aws::DynamoDB::Model::DescribeTableRequest dtr;
    dtr.SetTableName(name);
    const Aws::DynamoDB::Model::DescribeTableOutcome& result =  \
        pClient->DescribeTable(dtr);

    if (result.IsSuccess())
    {
        logdebug("[%s, %d] Found table %s to exist.\n", __FILENAME__, __LINE__, name.c_str());
        Aws::DynamoDB::Model::DescribeTableResult dtresult = result.GetResult();
        Aws::DynamoDB::Model::TableDescription td = dtresult.GetTable();

        logdebug("[%s, %d] status = %s\n", __FILENAME__, __LINE__,
                 Aws::DynamoDB::Model::TableStatusMapper::GetNameForTableStatus(td.GetTableStatus()).c_str());

        if (td.GetTableStatus() == Aws::DynamoDB::Model::TableStatus::ACTIVE)
            return true;

        return false;

    }
    else
    {
        if (!(result.GetError().GetErrorType() ==
              Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND))
            logerror("[%s, %d] %s. %s. %s\n", __FILENAME__, __LINE__,
                     result.GetError().GetExceptionName().c_str(),
                     result.GetError().GetRequestId().c_str(),
                     result.GetError().GetMessage().c_str());

        return false;
    }

}

void wait_for_table_active(std::string name, Aws::DynamoDB::DynamoDBClient * pClient)
{
    int ms = 100;
    do
    {
        std::this_thread::sleep_for( std::chrono::milliseconds(ms));
        if (ms < 2000)
            ms *= 2;
    } while (!table_active(name, pClient));
}

void wait_for_table_gone(std::string name, Aws::DynamoDB::DynamoDBClient * pClient)
{
    int ms = 100;
    do
    {
        std::this_thread::sleep_for( std::chrono::milliseconds(ms));
        if (ms < 2000)
            ms *= 2;
    } while (table_exists(name, pClient));
}

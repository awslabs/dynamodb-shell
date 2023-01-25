// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * update_table.cpp
 *
 * command handler for update_table
 *
 * Amrith - 2022-08-15
 */

#include "ddbsh.hpp"
#include "update_table.hpp"
#include "logging.hpp"
#include "parser-defs.hpp"
#include "strip_newlines.hpp"

using namespace ddbsh;

int CUpdateTableCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    if (explaining())
    {
        printf("UpdateTable(%s)\n", explain_string(m_update_table_request->SerializePayload()).c_str());
    }
    else
    {
        const Aws::DynamoDB::Model::UpdateTableOutcome & result =
            p_dynamoDBClient->UpdateTable(*m_update_table_request);

        if (!result.IsSuccess())
        {
            logerror("Update Table failed. %s. %s. %s\n",
                     result.GetError().GetExceptionName().c_str(),
                     result.GetError().GetRequestId().c_str(),
                     result.GetError().GetMessage().c_str());
            return -1;
        }

        printf("ALTER\n");
    }
    return 0;
}

CUpdateTableCommand::~CUpdateTableCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
    delete m_update_table_request;
}

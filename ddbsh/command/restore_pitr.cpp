// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * restore_pitr.cpp
 *
 * command handler for restore pitr command
 *
 * Amrith - 2022-08-15
 */

#include "ddbsh.hpp"
#include "strip_newlines.hpp"
#include "restore_pitr.hpp"
#include "logging.hpp"

#include "aws-includes.hpp"

using namespace ddbsh;

int CRestorePITRCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    Aws::DynamoDB::Model::RestoreTableToPointInTimeRequest request;
    request.SetSourceTableName(m_table_name);
    request.SetTargetTableName(m_target_name);
    request.SetRestoreDateTime(m_pit);

    if (explaining())
    {
        printf("RestoreTableToPointInTimeRequest(%s)\n", explain_string(request.SerializePayload()).c_str());
    }
    else
    {
        Aws::DynamoDB::Model::RestoreTableToPointInTimeOutcome outcome = p_dynamoDBClient->RestoreTableToPointInTime(request);

        if (!outcome.IsSuccess())
        {
            logerror("[%s, %d] %s. %s. %s\n", __FILENAME__, __LINE__,
                     outcome.GetError().GetExceptionName().c_str(),
                     outcome.GetError().GetRequestId().c_str(),
                     outcome.GetError().GetMessage().c_str());
        }
        else
        {
            printf("RESTORE\n");
        }

    }

    return 0;
}

CRestorePITRCommand::~CRestorePITRCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
}


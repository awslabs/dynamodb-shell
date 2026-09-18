// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * pitr.cpp
 *
 * command handler for pitr
 *
 * Amrith - 2022-08-15
 */

#include "ddbsh.hpp"
#include "pitr.hpp"
#include "logging.hpp"
#include "strip_newlines.hpp"
#include "aws-includes.hpp"

using namespace ddbsh;

int CUpdatePITRCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    Aws::DynamoDB::Model::UpdateContinuousBackupsRequest request;
    Aws::DynamoDB::Model::PointInTimeRecoverySpecification pitrspec;
    pitrspec.SetPointInTimeRecoveryEnabled(m_pitr);
    request.SetTableName(m_table_name);
    request.SetPointInTimeRecoverySpecification(pitrspec);

    if (explaining())
    {
        printf("UpdateContinuousBackupsRequest(%s)\n", explain_string(request.SerializePayload()).c_str());
        return 0;
    }

    Aws::DynamoDB::Model::UpdateContinuousBackupsOutcome outcome = p_dynamoDBClient->UpdateContinuousBackups(request);
    if (!outcome.IsSuccess())
    {
        logerror("Setting PITR failed. %s. %s.\n\t%s\n",
                 outcome.GetError().GetExceptionName().c_str(),
                 outcome.GetError().GetRequestId().c_str(),
                 outcome.GetError().GetMessage().c_str());

        return 1;
    }
    else
    {
        printf("ALTER\n");
    }

    return 0;
}

CUpdatePITRCommand::~CUpdatePITRCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
}


// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * drop_backup.cpp
 *
 * command handler for drop_backup
 *
 * Amrith - 2022-08-15
 */

#include "ddbsh.hpp"
#include "drop_backup.hpp"
#include "logging.hpp"

#include "aws-includes.hpp"
#include "strip_newlines.hpp"

using namespace ddbsh;

int CDropBackupCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    Aws::DynamoDB::Model::DeleteBackupRequest request;

    request.SetBackupArn(m_backup_arn);

    if (explaining())
    {
        printf("DescribeBackup(%s)\n", explain_string(request.SerializePayload()).c_str());
    }
    else
    {
        const Aws::DynamoDB::Model::DeleteBackupOutcome outcome = p_dynamoDBClient->DeleteBackup(request);

        if(!outcome.IsSuccess())
        {
            logerror("Drop Backup Failed. %s. %s.\n\t%s\n",
                     outcome.GetError().GetExceptionName().c_str(),
                     outcome.GetError().GetRequestId().c_str(),
                     outcome.GetError().GetMessage().c_str());
            return -1;
        }

        printf("DROP BACKUP\n");
    }

    return 0;
}

CDropBackupCommand::~CDropBackupCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
}


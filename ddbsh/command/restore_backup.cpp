// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * restore_backup.cpp
 *
 * command handler for restore_backup
 *
 * Amrith - 2022-08-15
 */

#include "ddbsh.hpp"
#include "restore_backup.hpp"
#include "logging.hpp"

#include "aws-includes.hpp"
#include "strip_newlines.hpp"

using namespace ddbsh;

int CRestoreBackupCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();
    Aws::DynamoDB::Model::RestoreTableFromBackupRequest request;

    request.SetTargetTableName(m_table_name);
    request.SetBackupArn(m_backup_arn);

    if (explaining())
    {
        printf("RestoreTableFromBackup(%s)\n", explain_string(request.SerializePayload()).c_str());
    }
    else
    {
        const Aws::DynamoDB::Model::RestoreTableFromBackupOutcome outcome = p_dynamoDBClient->RestoreTableFromBackup(request);

        if(!outcome.IsSuccess())
        {
            logerror("Restore Backup Failed. %s. %s.\n\t%s\n",
                     outcome.GetError().GetExceptionName().c_str(),
                     outcome.GetError().GetRequestId().c_str(),
                     outcome.GetError().GetMessage().c_str());
            return -1;
        }

        printf("RESTORE\n");
    }

    return 0;
}

CRestoreBackupCommand::~CRestoreBackupCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
}


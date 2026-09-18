// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * create_backup.cpp
 *
 * command handler for create_backup
 *
 * Amrith - 2022-08-15
 */

#include "ddbsh.hpp"
#include "create_backup.hpp"
#include "logging.hpp"

#include "aws-includes.hpp"
#include "strip_newlines.hpp"

using namespace ddbsh;

int CCreateBackupCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    Aws::DynamoDB::Model::CreateBackupRequest request;
    request.SetTableName(m_table_name);
    request.SetBackupName(m_backup_name);

    if (explaining())
    {
        printf("CreateBackup(%s)\n", explain_string(request.SerializePayload()).c_str());
    }
    else
    {
        const Aws::DynamoDB::Model::CreateBackupOutcome outcome = p_dynamoDBClient->CreateBackup(request);

        if(!outcome.IsSuccess())
        {
            logerror("Create Backup Failed. %s. %s.\n\t%s\n",
                     outcome.GetError().GetExceptionName().c_str(),
                     outcome.GetError().GetRequestId().c_str(),
                     outcome.GetError().GetMessage().c_str());
            return -1;
        }

        printf("BACKUP\n");
    }

    return 0;
}

CCreateBackupCommand::~CCreateBackupCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
}


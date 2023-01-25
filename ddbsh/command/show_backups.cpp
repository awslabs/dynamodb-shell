// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * show_backups.cpp
 *
 * command handler for show_backups
 *
 * Amrith - 2022-08-15
 */

#include "ddbsh.hpp"
#include "show_backups.hpp"
#include "logging.hpp"

#include "aws-includes.hpp"
#include "strip_newlines.hpp"

using namespace ddbsh;

int CShowBackupsCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();
    Aws::DynamoDB::Model::ListBackupsRequest request;
    if (!m_table_name.empty())
        request.SetTableName(m_table_name);

    if (!m_backup_type.empty())
        request.SetBackupType(Aws::DynamoDB::Model::BackupTypeFilterMapper::GetBackupTypeFilterForName(m_backup_type));

    request.SetLimit(100);

    do
    {
        if (explaining())
        {
            printf("ListBackups(%s)\n", explain_string(request.SerializePayload()).c_str());
        }

        const Aws::DynamoDB::Model::ListBackupsOutcome &outcome = p_dynamoDBClient->ListBackups(request);

        if (outcome.IsSuccess())
        {
            if (!explaining())
            {
                for (const auto &s : outcome.GetResult().GetBackupSummaries())
                {
                    // s is a BackupSummary
                    printf("Table: %s, Backup: %s, Status: %s, ARN: %s, On: %s, Expires: %s\n",
                           s.GetTableName().c_str(), s.GetBackupName().c_str(),
                           Aws::DynamoDB::Model::BackupStatusMapper::GetNameForBackupStatus(s.GetBackupStatus()).c_str(),
                           s.GetBackupArn().c_str(),
                           s.GetBackupCreationDateTime().ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str(),
                           s.GetBackupExpiryDateTime().ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str());
                }
            }
        }
        else
        {
            logerror("ListBackups Failed. %s. %s.\n\t%s\n",
                     outcome.GetError().GetExceptionName().c_str(),
                     outcome.GetError().GetRequestId().c_str(),
                     outcome.GetError().GetMessage().c_str());
            return -1;
        }

        request.SetExclusiveStartBackupArn(outcome.GetResult().GetLastEvaluatedBackupArn());
    } while(!request.GetExclusiveStartBackupArn().empty());

    return 0;
}

CShowBackupsCommand::~CShowBackupsCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
}


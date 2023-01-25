// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * describe_backup.cpp
 *
 * command handler for describe_backup
 *
 * Amrith - 2022-08-15
 */

#include "ddbsh.hpp"
#include "describe_backup.hpp"
#include "logging.hpp"

#include "aws-includes.hpp"
#include "strip_newlines.hpp"

using namespace ddbsh;

int CDescribeBackupCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    Aws::DynamoDB::Model::DescribeBackupRequest request;
    request.SetBackupArn(m_backup_arn);

    if (explaining())
    {
        printf("DescribeBackup(%s)\n", explain_string(request.SerializePayload()).c_str());
    }
    else
    {
        const Aws::DynamoDB::Model::DescribeBackupOutcome outcome = p_dynamoDBClient->DescribeBackup(request);

        if (outcome.IsSuccess())
        {
            const Aws::DynamoDB::Model::BackupDescription &bd = outcome.GetResult().GetBackupDescription();
            const Aws::DynamoDB::Model::SourceTableDetails &std = bd.GetSourceTableDetails();

            printf("Table Name: %s\n", std.GetTableName().c_str());
            printf("Tabld Id: %s\n", std.GetTableId().c_str());
            printf("Table Arn: %s\n", std.GetTableArn().c_str());
            printf("Table Size (bytes): %lld\n", std.GetTableSizeBytes());
            printf("Item Count: %lld\n", std.GetItemCount());

            Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> ks = std.GetKeySchema();
            std::string key;
            for (const auto& kse: ks)
            {
                if (!key.empty())
                    key = key + ", ";

                key = key + Aws::DynamoDB::Model::KeyTypeMapper::GetNameForKeyType(kse.GetKeyType()) + " " + kse.GetAttributeName();
            }
            printf("Table Key: ( %s )\n", key.c_str());

            printf("Table Creation Date/Time: %s\n",
                   std.GetTableCreationDateTime().ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str());
            if (std.BillingModeHasBeenSet())
                printf("Billing Mode: %s\n",
                       Aws::DynamoDB::Model::BillingModeMapper::GetNameForBillingMode(std.GetBillingMode()).c_str());


            if (std.ProvisionedThroughputHasBeenSet())
                printf("Provisioned Throughput: %lld RCU, %lld WCU\n",
                       std.GetProvisionedThroughput().GetReadCapacityUnits(),
                       std.GetProvisionedThroughput().GetWriteCapacityUnits());


            const Aws::DynamoDB::Model::BackupDetails bdet = bd.GetBackupDetails();
            printf("Backup Name: %s\n", bdet.GetBackupName().c_str());
            printf("Backup Arn: %s\n", bdet.GetBackupArn().c_str());
            printf("Backup Size (bytes): %lld\n", bdet.GetBackupSizeBytes());
            printf("Backup Status: %s\n",
                   Aws::DynamoDB::Model::BackupStatusMapper::GetNameForBackupStatus(bdet.GetBackupStatus()).c_str());
            printf("Backup Type: %s\n", Aws::DynamoDB::Model::BackupTypeMapper::GetNameForBackupType(bdet.GetBackupType()).c_str());
            printf("Backup Creation Date/Time: %s\n",
                   bdet.GetBackupCreationDateTime().ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str());
            printf("Backup Expiry Date/Time: %s\n",
                   bdet.GetBackupExpiryDateTime().ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str());
        }
        else
        {
            logerror("DescribeBackup on \"%s\" Failed. %s. %s.\n\t%s\n",
                     m_backup_arn.c_str(),
                     outcome.GetError().GetExceptionName().c_str(),
                     outcome.GetError().GetRequestId().c_str(),
                     outcome.GetError().GetMessage().c_str());
            return -1;
        }
    }

    return 0;
}

CDescribeBackupCommand::~CDescribeBackupCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
}


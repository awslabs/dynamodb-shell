// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * show_limits.cpp
 *
 * command handler for show_limits
 *
 * Amrith - 2022-08-15
 */

#include "ddbsh.hpp"
#include "show_limits.hpp"
#include "logging.hpp"
#include "describe_helper.hpp"

#include "aws-includes.hpp"
#include "strip_newlines.hpp"

using namespace ddbsh;

int CShowLimitsCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    Aws::DynamoDB::Model::DescribeLimitsRequest request;

    if (explaining())
    {
        printf("DescribeLimits(%s)\n", explain_string(request.SerializePayload()).c_str());
    }
    else
    {
        const Aws::DynamoDB::Model::DescribeLimitsOutcome &outcome = p_dynamoDBClient->DescribeLimits(request);

        if (outcome.IsSuccess())
        {
            printf("Region: %s\n", CDDBSh::getInstance()->getRegion().c_str());
            printf("Account Max (RCU, WCU): %lld, %lld\n",
                   outcome.GetResult().GetAccountMaxReadCapacityUnits(),
                   outcome.GetResult().GetAccountMaxWriteCapacityUnits());
            printf("Table Max (RCU, WCU):  %lld, %lld\n",
                   outcome.GetResult().GetTableMaxReadCapacityUnits(),
                   outcome.GetResult().GetTableMaxWriteCapacityUnits());
        }
        else
        {
            logerror("DescribeLimits Failed. %s. %s\n\t%s\n",
                     outcome.GetError().GetExceptionName().c_str(),
                     outcome.GetError().GetRequestId().c_str(),
                     outcome.GetError().GetMessage().c_str());
            return -1;
        }
    }

    Aws::DynamoDB::Model::ListTablesRequest listTablesRequest;
    listTablesRequest.SetLimit(100);

    long long rcus = 0, wcus = 0;
    long long gsircus = 0, gsiwcus = 0;

    logdebug("[%s, %d] Listing tables to compute total RCU, WCU\n", __FILENAME__, __LINE__);

    do
    {
        if (explaining())
        {
            printf("ListTables(%s)\n", explain_string(listTablesRequest.SerializePayload()).c_str());
        }

        const Aws::DynamoDB::Model::ListTablesOutcome& lto = p_dynamoDBClient->ListTables(listTablesRequest);

        if (!lto.IsSuccess())
        {
            logerror("ListTables Failed. %s. %s.\n\t%s\n",
                     lto.GetError().GetExceptionName().c_str(),
                     lto.GetError().GetRequestId().c_str(),
                     lto.GetError().GetMessage().c_str());
            return -1;
        }
        else if (!explaining())
        {
            for (const auto& s : lto.GetResult().GetTableNames())
            {
                CDescribeHelper dh(s, p_dynamoDBClient);
                Aws::DynamoDB::Model::TableDescription td = dh.GetDTR().GetTable();

                logdebug("[%s, %d] Looking up table %s (type: %s)\n", __FILENAME__, __LINE__, td.GetTableName().c_str(),
                         Aws::DynamoDB::Model::BillingModeMapper::GetNameForBillingMode(
                             td.GetBillingModeSummary().GetBillingMode()).c_str());

                if ((td.BillingModeSummaryHasBeenSet() &&
                     td.GetBillingModeSummary().GetBillingMode() == Aws::DynamoDB::Model::BillingMode::PROVISIONED) ||
                    td.ProvisionedThroughputHasBeenSet())
                {
                    Aws::DynamoDB::Model::ProvisionedThroughputDescription ptd = td.GetProvisionedThroughput();

                    logdebug("[%s, %d] Table %s is provisioned (%lld, %lld)\n", __FILENAME__, __LINE__,
                             dh.GetDTR().GetTable().GetTableName().c_str(),
                             ptd.GetReadCapacityUnits(), ptd.GetWriteCapacityUnits());

                    rcus += ptd.GetReadCapacityUnits();
                    wcus += ptd.GetWriteCapacityUnits();

                    // does the table have GSIs?
                    Aws::Vector<Aws::DynamoDB::Model::GlobalSecondaryIndexDescription> gsis = td.GetGlobalSecondaryIndexes();
                    if (gsis.size() > 0)
                    {
                        for (const auto& gsi : gsis)
                        {
                            Aws::DynamoDB::Model::ProvisionedThroughputDescription ptd = gsi.GetProvisionedThroughput();
                            gsircus += ptd.GetReadCapacityUnits();
                            gsiwcus += ptd.GetWriteCapacityUnits();
                            logdebug("[%s, %d] Table %s has GSI (%lld, %lld)\n", __FILENAME__, __LINE__,
                                     dh.GetDTR().GetTable().GetTableName().c_str(),
                                     ptd.GetReadCapacityUnits(), ptd.GetWriteCapacityUnits());
                        }
                    }
                }
            }
        }

        listTablesRequest.SetExclusiveStartTableName(lto.GetResult().GetLastEvaluatedTableName());
    } while (!listTablesRequest.GetExclusiveStartTableName().empty());

    if (!explaining())
    {
        printf("Total Tables (RCU, WCU): %lld, %lld\n", rcus, wcus);
        printf("Total GSI (RCU, WCU): %lld, %lld\n", gsircus, gsiwcus);
        printf("Total (RCU, WCU): %lld, %lld\n", rcus + gsircus, wcus + gsiwcus);
    }
    return 0;
}

CShowLimitsCommand::~CShowLimitsCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
}


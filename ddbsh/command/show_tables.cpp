// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * show_tables.cpp
 *
 * command handler for show tables
 *
 * Amrith - 2022-08-11
 */

#include "ddbsh.hpp"
#include "show_tables.hpp"
#include "logging.hpp"
#include "aws-includes.hpp"
#include "describe_helper.hpp"
#include "strip_newlines.hpp"
#include <regex>

using namespace ddbsh;

int CShowTablesCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();
    Aws::DynamoDB::Model::ListTablesRequest listTablesRequest;
    listTablesRequest.SetLimit(100);

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
                if (m_regexp.empty() ||
                    std::regex_match(s, std::regex(m_regexp)))
                {
                    if (!m_brief)
                    {
                        CDescribeHelper dh(s, p_dynamoDBClient);

                        if (dh.has_dtr() && dh.has_ttl())
                        {
                            Aws::DynamoDB::Model::TableDescription td = dh.GetDTR().GetTable();
                            Aws::DynamoDB::Model::TimeToLiveDescription ttld = dh.GetTTL().GetTimeToLiveDescription();

                            std::string ttl = Aws::DynamoDB::Model::TimeToLiveStatusMapper::GetNameForTimeToLiveStatus(
                                    dh.GetTTL().GetTimeToLiveDescription(). GetTimeToLiveStatus());

                            if ((dh.GetTTL().GetTimeToLiveDescription().GetTimeToLiveStatus() ==
                                 Aws::DynamoDB::Model::TimeToLiveStatus::ENABLED) ||
                                (dh.GetTTL().GetTimeToLiveDescription().GetTimeToLiveStatus() ==
                                 Aws::DynamoDB::Model::TimeToLiveStatus::ENABLING))
                                ttl += " (" + dh.GetTTL().GetTimeToLiveDescription().GetAttributeName() + ")";

                            std::string billing_mode;
                            std::string table_class;

                            if (td.BillingModeSummaryHasBeenSet())
                                billing_mode = Aws::DynamoDB::Model::BillingModeMapper::GetNameForBillingMode(
                                    td.GetBillingModeSummary().GetBillingMode());
                            else if (td.ProvisionedThroughputHasBeenSet())
                                billing_mode = Aws::DynamoDB::Model::BillingModeMapper::GetNameForBillingMode(
                                    Aws::DynamoDB::Model::BillingMode::PROVISIONED);
                            else
                                billing_mode = Aws::DynamoDB::Model::BillingModeMapper::GetNameForBillingMode(
                                    Aws::DynamoDB::Model::BillingMode::PAY_PER_REQUEST) + "*";

                            table_class = td.TableClassSummaryHasBeenSet() ?
                                Aws::DynamoDB::Model::TableClassMapper::GetNameForTableClass(
                                    td.GetTableClassSummary().GetTableClass()) :
                                Aws::DynamoDB::Model::TableClassMapper::GetNameForTableClass(
                                    Aws::DynamoDB::Model::TableClass::STANDARD) + "*";

                            printf("%s | %s | %s | %s | %s | %s | "
                                   "TTL %s | GSI: %ld | LSI : %ld |\n",
                                   s.c_str(),
                                   Aws::DynamoDB::Model:: TableStatusMapper::GetNameForTableStatus(td.GetTableStatus()).c_str(),
                                   billing_mode.c_str(),
                                   table_class.c_str(),
                                   td.GetTableId().c_str(),
                                   td.GetTableArn().c_str(),
                                   ttl.c_str(),
                                   td.GetGlobalSecondaryIndexes().size(),
                                   td.GetLocalSecondaryIndexes().size());
                        }
                        else
                        {
                            printf("%s\n", s.c_str());
                        }
                    }
                    else
                    {
                        printf("%s\n", s.c_str());
                    }
                }
            }

            listTablesRequest.SetExclusiveStartTableName(lto.GetResult().GetLastEvaluatedTableName());
        }
    } while (!listTablesRequest.GetExclusiveStartTableName().empty());

    return 0;
}

CShowTablesCommand::~CShowTablesCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
}

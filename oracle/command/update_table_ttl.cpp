// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * update_table_ttl.cpp
 *
 * command handler for update_table_ttl
 *
 * Amrith - 2022-08-15
 */

#include "ddbsh.hpp"
#include "update_table_ttl.hpp"
#include "logging.hpp"
#include "describe_helper.hpp"
#include "strip_newlines.hpp"

#include "aws-includes.hpp"

using namespace ddbsh;

int CUpdateTableTTLCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    CDescribeHelper dh(m_table_name, p_dynamoDBClient);

    Aws::DynamoDB::Model::UpdateTimeToLiveRequest uttlr;
    Aws::DynamoDB::Model::TimeToLiveSpecification ttls;

    uttlr.SetTableName(m_table_name);

    if (!dh.has_ttl())
        return -1;

    ttls.SetEnabled(m_enabled);

    // if we are where we want to be, do nothing.
    Aws::DynamoDB::Model::TimeToLiveDescription ttld = dh.GetTTL().GetTimeToLiveDescription();

    if ((ttld.GetTimeToLiveStatus() == Aws::DynamoDB::Model::TimeToLiveStatus::DISABLED) && (m_enabled == false))
    {
        logdebug("[%s, %d] TTL is disabled already.\n", __FILENAME__, __LINE__);
        printf("ALTER\n");
        return 0;
    }

    if (m_enabled)
    {
        logdebug("[%s, %d] Will enable TTL on %s with attribute %s\n",
                 __FILENAME__, __LINE__, m_table_name.c_str(),
                 m_attribute.c_str());
        ttls.SetAttributeName(m_attribute);
    }
    else
        ttls.SetAttributeName(dh.GetTTL().GetTimeToLiveDescription().GetAttributeName());

    uttlr.SetTimeToLiveSpecification(ttls);

    if (explaining())
    {
        printf("UpdateTimeToLive(%s)\n", explain_string(uttlr.SerializePayload()).c_str());
    }
    else
    {
        const Aws::DynamoDB::Model::UpdateTimeToLiveOutcome& uttlo = p_dynamoDBClient->UpdateTimeToLive(uttlr);

        if (!uttlo.IsSuccess())
        {
            logerror("[%s, %d] Error updating TTL. %s. %s.\n\t%s\n",
                     __FILENAME__, __LINE__,
                     uttlo.GetError().GetExceptionName().c_str(),
                     uttlo.GetError().GetRequestId().c_str(),
                     uttlo.GetError().GetMessage().c_str());
            return -1;
        }

        logdebug("[%s, %d] Done alter.\n", __FILENAME__, __LINE__);
        printf ("ALTER\n");
    }

    return 0;
}

CUpdateTableTTLCommand::~CUpdateTableTTLCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
}


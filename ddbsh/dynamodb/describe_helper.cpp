// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * describe-helper.cpp
 *
 * wraps multiple calls and returns multiple things.
 *
 * TableDescription (DescribeTable)
 * TimeToLiveDescription (DescribeTimeToLive)
 *
 * Amrith. 2020-08-20
 */

#include "logging.hpp"
#include "aws-includes.hpp"
#include "describe_helper.hpp"
#include "strip_newlines.hpp"

void CDescribeHelper::__constructor_helper(std::string table_name, Aws::DynamoDB::DynamoDBClient * pClient, bool explain)
{
    m_explain = explain;
    m_table_name = table_name;

    Aws::DynamoDB::Model::DescribeTableRequest dtr;
    dtr.SetTableName(table_name);

    if (m_explain)
    {
        printf("DescribeTable(%s)\n", strip_newlines(dtr.SerializePayload()).c_str());
    }

    const Aws::DynamoDB::Model::DescribeTableOutcome& dtrresult = pClient->DescribeTable(dtr);

    m_dtr_has_been_set = false;
    if (dtrresult.IsSuccess())
    {
        m_dtr = dtrresult.GetResult();
        m_dtr_has_been_set = true;
    }
    else
    {
        logdebug("[%s, %d] %s. %s. %s\n", __FILENAME__, __LINE__,
                 dtrresult.GetError().GetExceptionName().c_str(),
                 dtrresult.GetError().GetRequestId().c_str(),
                 dtrresult.GetError().GetMessage().c_str());
        return;
    }

    Aws::DynamoDB::Model::DescribeTimeToLiveRequest dttlr;
    dttlr.SetTableName(table_name);

    if (m_explain)
    {
        printf("DescribeTimeToLive(%s)\n", strip_newlines(dttlr.SerializePayload()).c_str());
    }

    const Aws::DynamoDB::Model::DescribeTimeToLiveOutcome &dttlresult = pClient->DescribeTimeToLive(dttlr);

    m_dttl_has_been_set = false;
    if (dttlresult.IsSuccess())
    {
        m_dttl = dttlresult.GetResult();
        m_dttl_has_been_set = true;
    }
    else
    {
        logdebug("[%s, %d] %s. %s. %s\n", __FILENAME__, __LINE__,
                 dttlresult.GetError().GetExceptionName().c_str(),
                 dttlresult.GetError().GetRequestId().c_str(),
                 dttlresult.GetError().GetMessage().c_str());
    }
}

CDescribeHelper::CDescribeHelper(std::string table_name, Aws::DynamoDB::DynamoDBClient * pClient)
{
    __constructor_helper(table_name, pClient, false);
}

CDescribeHelper::CDescribeHelper(std::string table_name, Aws::DynamoDB::DynamoDBClient * pClient, bool explain)
{
    __constructor_helper(table_name, pClient, explain);
}




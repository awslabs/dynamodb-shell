// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * describe-helper.hpp
 *
 * wraps multiple calls and returns multiple things.
 *
 * TableDescription (DescribeTable)
 * TimeToLiveDescription (DescribeTimeToLive)
 *
 * Amrith. 2020-08-20
 */

#ifndef __DESCRIBE_HELPER_DEFINED_HPP__
#define __DESCRIBE_HELPER_DEFINED_HPP__

#include "aws-includes.hpp"

class CDescribeHelper
{
public:
    void __constructor_helper(std::string table_name,
                         Aws::DynamoDB::DynamoDBClient * pClient,
                         bool explain);

    CDescribeHelper(std::string table_name,
                    Aws::DynamoDB::DynamoDBClient * pClient,
                    bool explain);
    CDescribeHelper(std::string table_name,
                    Aws::DynamoDB::DynamoDBClient * pClient);
    bool has_dtr() { return m_dtr_has_been_set; };
    Aws::DynamoDB::Model::DescribeTableResult& GetDTR() {return m_dtr; };
    bool has_ttl() { return m_dttl_has_been_set; };
    Aws::DynamoDB::Model::DescribeTimeToLiveResult& GetTTL(){ return m_dttl; };

private:
    bool m_explain;
    std::string m_table_name;
    bool m_dtr_has_been_set;
    Aws::DynamoDB::Model::DescribeTableResult m_dtr;
    bool m_dttl_has_been_set;
    Aws::DynamoDB::Model::DescribeTimeToLiveResult m_dttl;
};
#endif

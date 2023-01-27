// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * inodel_ddb_client_config.hpp
 *
 * a wrapper on Aws::Client::ClientConfiguration for ddbsh
 *
 * Amrith - 2022-08-11
 */

#ifndef __DDBSH_DDB_CLIENT_CONFIG_HPP_DEFINED__
#define __DDBSH_DDB_CLIENT_CONFIG_HPP_DEFINED__

#include <aws/dynamodb/DynamoDBClient.h>

namespace ddbsh
{
    class CDDBShDDBClientConfig : public Aws::DynamoDB::DynamoDBClientConfiguration
    {
    public:
        CDDBShDDBClientConfig(std::string region, std::string endpoint);
    };
};
#endif

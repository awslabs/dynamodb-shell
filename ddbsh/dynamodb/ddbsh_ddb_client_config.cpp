// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * inodel_ddb_client_config.cpp
 *
 * a wrapper on Aws::Client::ClientConfiguration for ddbsh
 *
 * Amrith - 2022-08-11
 */

#include "ddbsh_ddb_client_config.hpp"
#include "version.h"

using namespace ddbsh;

CDDBShDDBClientConfig::CDDBShDDBClientConfig(std::string region, std::string endpoint)
{
    char userAgent[128];
    strcat(userAgent, ddbsh_version());
    this->userAgent = userAgent;
    this->region = region;
    if (!endpoint.empty())
    {
        Aws::DynamoDB::DynamoDBClientConfiguration config;
        config.region = region;
        std::shared_ptr<Aws::DynamoDB::DynamoDBEndpointProviderBase> endpointProvider =
            Aws::MakeShared<Aws::DynamoDB::DynamoDBEndpointProvider>("ddbsh");
        endpointProvider->InitBuiltInParameters(config);
        endpointProvider->OverrideEndpoint(endpoint);
        Aws::Endpoint::ResolveEndpointOutcome resolvedEndpoint = endpointProvider->ResolveEndpoint({});
        this->endpointOverride = resolvedEndpoint.GetResult().GetURL();
    }
}

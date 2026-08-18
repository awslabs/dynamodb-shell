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
#include <cstdlib>

using namespace ddbsh;

CDDBShDDBClientConfig::CDDBShDDBClientConfig(std::string region, std::string endpoint)
{
    char userAgent[128];
    sprintf(userAgent, "ddbsh - version %s", ddbsh_version());
    this->userAgent = userAgent;
    this->region = region;

    // Honor AWS_CA_BUNDLE / AWS_CA_PATH so ddbsh can trust a private or
    // self-signed CA (for example a local DynamoDB-compatible endpoint served
    // over TLS with a self-signed certificate). The AWS SDK for C++ does not
    // read these itself, and ddbsh never set caFile, so previously the only way
    // to trust a custom endpoint's certificate was to modify the host's system
    // trust store. Setting caFile here makes the SDK's HTTP client pass the
    // bundle to curl (CURLOPT_CAINFO). This mirrors the AWS CLI's AWS_CA_BUNDLE.
    const char * ca_bundle = std::getenv("AWS_CA_BUNDLE");
    if (ca_bundle && *ca_bundle)
        this->caFile = ca_bundle;

    const char * ca_path = std::getenv("AWS_CA_PATH");
    if (ca_path && *ca_path)
        this->caPath = ca_path;

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

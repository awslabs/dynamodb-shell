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
    sprintf(userAgent, "ddbsh version %d.%d", VERSION_MAJOR, VERSION_MINOR);
    this->userAgent = userAgent;
    this->endpointOverride = endpoint;
    this->region = region;
}

// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * connect.cpp
 *
 * command handler for connect
 *
 * Amrith - 2022-08-10
 */

#include "ddbsh.hpp"
#include "connect.hpp"
#include "logging.hpp"
#include "strip_newlines.hpp"

using namespace ddbsh;

int CConnectCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);
    CDDBSh *  pInstance = CDDBSh::getInstance();

    pInstance->setRegionAndEndpoint(m_region, m_endpoint);

    printf("CONNECT\n");
    return 0;
}

CConnectCommand::CConnectCommand(std::string region, std::string endpoint)
{
    m_region = region;
    m_endpoint = endpoint;
}


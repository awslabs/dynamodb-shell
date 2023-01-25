// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * connect.hpp
 *
 * command handler for connect
 *
 * Amrith - 2022-08-10
 */

#ifndef __CONNECT_HPP_DEFINED__
#define __CONNECT_HPP_DEFINED__

#include <string>

#include "command.hpp"

namespace ddbsh
{
    class CConnectCommand: public CCommand
    {
    public:
        CConnectCommand(std::string region, std::string endpoint);
        int run();

    private:
        std::string m_region;
        std::string m_endpoint;
    };
}
#endif

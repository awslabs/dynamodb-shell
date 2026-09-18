// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * describe.hpp
 *
 * command handler for describe
 *
 * Amrith - 2022-08-15
 */

#ifndef __DESCRIBE_COMMAND_HPP_DEFINED__
#define __DESCRIBE_COMMAND_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{
    class CDescribeCommand: public CCommand
    {
    public:
        CDescribeCommand(std::string table) : m_table {table} {};
        int run();
        virtual ~CDescribeCommand();

    private:
        std::string m_table;
    };
}
#endif

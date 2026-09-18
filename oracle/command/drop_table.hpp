// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * show_tables.hpp
 *
 * command handler for show tables
 *
 * Amrith - 2022-08-11
 */

#ifndef __DROP_TABLE_COMMAND_HPP_DEFINED__
#define __DROP_TABLE_COMMAND_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{
    class CDropTableCommand: public CCommand
    {
    public:
        CDropTableCommand(std::string table, bool exists, bool nowait);
        int run();
        virtual ~CDropTableCommand();

    private:
        std::string m_table;
        bool m_exists;
        bool m_nowait;
    };
}
#endif

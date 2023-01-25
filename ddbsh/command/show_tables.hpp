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

#ifndef __SHOW_TABLES_COMMAND_HPP_DEFINED__
#define __SHOW_TABLES_COMMAND_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{
    class CShowTablesCommand: public CCommand
    {
    public:
        int run();
        virtual ~CShowTablesCommand();
        CShowTablesCommand(bool brief, char * regexp) :
            m_regexp { regexp }, m_brief { brief } {};

    private:
        std::string m_regexp;
        bool m_brief;
    };
}
#endif

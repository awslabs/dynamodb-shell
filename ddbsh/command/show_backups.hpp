// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * show_backups.hpp
 *
 * command handler for show_backups command
 *
 * Amrith - 2022-08-15
 */

#ifndef __SHOW_BACKUPS_COMMAND_HPP_DEFINED__
#define __SHOW_BACKUPS_COMMAND_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{
    class CShowBackupsCommand: public CCommand
    {
    public:
        CShowBackupsCommand(std::string backup_type, std::string table_name) :
            m_backup_type {backup_type},
            m_table_name {table_name} {};
        int run();
        virtual ~CShowBackupsCommand();

    private:
        std::string m_backup_type;
        std::string m_table_name;
    };
}
#endif

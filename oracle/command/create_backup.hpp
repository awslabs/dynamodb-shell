// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * create_backup.hpp
 *
 * command handler for create_backup command
 *
 * Amrith - 2022-08-15
 */

#ifndef __CREATE_BACKUP_COMMAND_HPP_DEFINED__
#define __CREATE_BACKUP_COMMAND_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{
    class CCreateBackupCommand: public CCommand
    {
    public:
        CCreateBackupCommand(std::string table_name, std::string backup_name) :
            m_table_name {table_name},
            m_backup_name {backup_name} {};
        int run();
        virtual ~CCreateBackupCommand();

    private:
        std::string m_table_name;
        std::string m_backup_name;
    };
}
#endif

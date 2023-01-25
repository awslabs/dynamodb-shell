// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * restore_backup.hpp
 *
 * command handler for restore_backup command
 *
 * Amrith - 2022-08-15
 */

#ifndef __RESTORE_BACKUP_COMMAND_HPP_DEFINED__
#define __RESTORE_BACKUP_COMMAND_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{
    class CRestoreBackupCommand: public CCommand
    {
    public:
        CRestoreBackupCommand(std::string table, std::string arn) :
            m_table_name { table },
            m_backup_arn { arn } {};
        int run();
        virtual ~CRestoreBackupCommand();

    private:
        std::string m_table_name;
        std::string m_backup_arn;
    };
}
#endif

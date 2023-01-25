// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * describe_backup.hpp
 *
 * command handler for describe_backup command
 *
 * Amrith - 2022-08-15
 */

#ifndef __DESCRIBE_BACKUP_COMMAND_HPP_DEFINED__
#define __DESCRIBE_BACKUP_COMMAND_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{
    class CDescribeBackupCommand: public CCommand
    {
    public:
        CDescribeBackupCommand(std::string arn) :
            m_backup_arn {arn} {};
        int run();
        virtual ~CDescribeBackupCommand();

    private:
        std::string m_backup_arn;
    };
}
#endif

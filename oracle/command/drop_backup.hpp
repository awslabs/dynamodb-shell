// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * drop_backup.hpp
 *
 * command handler for drop_backup command
 *
 * Amrith - 2022-08-15
 */

#ifndef __DROP_BACKUP_COMMAND_HPP_DEFINED__
#define __DROP_BACKUP_COMMAND_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{

    class CDropBackupCommand: public CCommand
    {
    public:
        CDropBackupCommand(std::string backup_arn) :
            m_backup_arn { backup_arn }{};
        int run();
        virtual ~CDropBackupCommand();

    private:
        std::string m_backup_arn;
    };

}
#endif

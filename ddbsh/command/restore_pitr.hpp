// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * restore_pitr.hpp
 *
 * command handler for restore pitr command
 *
 * Amrith - 2022-11-21
 */

#ifndef __RESTORE_PITR_COMMAND_HPP_DEFINED__
#define __RESTORE_PITR_COMMAND_HPP_DEFINED__

#include "command.hpp"
#include "aws-includes.hpp"

namespace ddbsh
{
    class CRestorePITRCommand: public CCommand
    {
    public:
        CRestorePITRCommand(std::string table, std::string timestamp, std::string target) {
            m_table_name = table;
            m_target_name = target;
            m_pit = Aws::Utils::DateTime(timestamp,  Aws::Utils::DateFormat::ISO_8601);
            logdebug("[%s, %d] In %s for table = %s, to timestamp = %s, target = %s\n", __FILENAME__, __LINE__, __FUNCTION__,
                     m_table_name.c_str(), m_pit.ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str(),
                     m_target_name.c_str());
        };

        int run();
        virtual ~CRestorePITRCommand();

    private:
        std::string m_table_name;
        std::string m_target_name;
        Aws::Utils::DateTime m_pit;
    };
}
#endif

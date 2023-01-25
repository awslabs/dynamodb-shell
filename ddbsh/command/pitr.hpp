// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * pitr.hpp
 *
 * command handler for PITR
 *
 * Amrith - 2022-11-21
 */

#ifndef __PITR_COMMAND_HPP_DEFINED__
#define __PITR_COMMAND_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{
    class CUpdatePITRCommand: public CCommand
    {
    public:
        CUpdatePITRCommand(std::string table, bool value) {
            m_table_name = table;
            m_pitr = value;
        };
        int run();
        virtual ~CUpdatePITRCommand();

    private:
        std::string m_table_name;
        bool m_pitr;
    };
}
#endif

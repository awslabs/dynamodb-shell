// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * update_table_ttl.hpp
 *
 * command handler for update_table_ttl command
 *
 * Amrith - 2022-08-15
 */

#ifndef __UPDATE_TABLE_TTL_COMMAND_HPP_DEFINED__
#define __UPDATE_TABLE_TTL_COMMAND_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{
    class CUpdateTableTTLCommand: public CCommand
    {
    public:
        CUpdateTableTTLCommand(std::string table_name,
                               bool enabled) :
            m_table_name { table_name },
            m_enabled { enabled } { m_attribute = "" ;};
        CUpdateTableTTLCommand(std::string table_name,
                               bool enabled,
                               std::string attribute) :
            m_table_name { table_name },
            m_attribute { attribute },
            m_enabled {enabled }{};

        int run();
        virtual ~CUpdateTableTTLCommand();

    private:
        std::string m_table_name;
        bool m_enabled;
        std::string m_attribute;
    };
}
#endif

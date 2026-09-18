// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * show_create_table.hpp
 *
 * command handler for show create table
 *
 * Amrith - 2023-05-05
 */

#ifndef __SHOW_CREATE_TABLE_COMMAND_HPP_DEFINED__
#define __SHOW_CREATE_TABLE_COMMAND_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{
    class CShowCreateTableCommand: public CCommand
    {
    public:
        int run();
        CShowCreateTableCommand(bool not_exists, bool nowait, std::string table_name) {
            m_table_name = table_name;
            m_nowait = nowait;
            m_not_exists = not_exists;
        };

    private:
        std::string m_table_name;
        bool m_not_exists;
        bool m_nowait;
        std::string get_key_description(const std::vector<Aws::DynamoDB::Model::KeySchemaElement> schema);
        std::string billing_mode_provisioned(Aws::DynamoDB::Model::ProvisionedThroughputDescription ptd);
        std::string billing_mode_on_demand();
        std::string show_gsis(const Aws::Vector<Aws::DynamoDB::Model::GlobalSecondaryIndexDescription> gsis,
                                bool table_on_demand);
        std::string show_lsis(const Aws::Vector<Aws::DynamoDB::Model::LocalSecondaryIndexDescription> gsis);
        std::string stream_specification(const Aws::DynamoDB::Model::StreamSpecification stream);
    };
};

#endif

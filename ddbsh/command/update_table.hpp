// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * update_table.hpp
 *
 * command handler for update_table command
 *
 * Amrith - 2022-08-15
 */

#ifndef __UPDATE_TABLE_COMMAND_HPP_DEFINED__
#define __UPDATE_TABLE_COMMAND_HPP_DEFINED__

#include "command.hpp"
#include "parser-defs.hpp"
#include "allocation.hpp"

namespace ddbsh
{
    class CUpdateTableCommand: public CCommand
    {
    public:
        CUpdateTableCommand(std::string table_name) {
            m_update_table_request = NEW Aws::DynamoDB::Model::UpdateTableRequest;
            m_update_table_request->SetTableName(table_name);
        };

        CUpdateTableCommand(std::string table_name,
                            billing_mode_and_throughput_t * billing_mode_and_throughput) :
            CUpdateTableCommand(table_name) {
            if (billing_mode_and_throughput->mode == Aws::DynamoDB::Model::BillingMode::PROVISIONED)
                m_update_table_request->SetProvisionedThroughput(billing_mode_and_throughput->throughput);

            m_update_table_request->SetBillingMode(billing_mode_and_throughput->mode);

            FREE(billing_mode_and_throughput);
        };

        CUpdateTableCommand(std::string table_name,
                            Aws::DynamoDB::Model::SSESpecification * sse_specification) :
            CUpdateTableCommand(table_name) {
            m_update_table_request->SetSSESpecification(*sse_specification);
            delete sse_specification;
        };

        CUpdateTableCommand(std::string table_name,
                            Aws::DynamoDB::Model::TableClass table_class) :
            CUpdateTableCommand(table_name) {
            m_update_table_request->SetTableClass(table_class);
        };

        CUpdateTableCommand(std::string table_name,
                            Aws::DynamoDB::Model::StreamSpecification * stream_specification) :
            CUpdateTableCommand(table_name) {
            m_update_table_request->SetStreamSpecification(*stream_specification);
            delete stream_specification;
        };

        CUpdateTableCommand(std::string table_name,
                            Aws::Vector<Aws::DynamoDB::Model::AttributeDefinition> * attribute_definitions,
                            Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate * gsiu ):
            CUpdateTableCommand(table_name) {
            m_update_table_request->SetAttributeDefinitions(*attribute_definitions);
            m_update_table_request->AddGlobalSecondaryIndexUpdates(*gsiu);
            delete attribute_definitions;
            delete gsiu;
        };

        CUpdateTableCommand(std::string table_name,
                            billing_mode_and_throughput_t * billing_mode_and_throughput,
                            Aws::Vector<Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate> * gsiu ):
            CUpdateTableCommand(table_name, billing_mode_and_throughput) {
            m_update_table_request->SetGlobalSecondaryIndexUpdates(*gsiu);
            delete gsiu;
        };

        CUpdateTableCommand(std::string table_name,
                            Aws::Vector<Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate> * gsiu ):
            CUpdateTableCommand(table_name) {
            m_update_table_request->SetGlobalSecondaryIndexUpdates(*gsiu);
            delete gsiu;
        };

        CUpdateTableCommand(std::string table_name,
                            Aws::DynamoDB::Model::GlobalSecondaryIndexUpdate * gsiu ):
            CUpdateTableCommand(table_name) {
            m_update_table_request->AddGlobalSecondaryIndexUpdates(*gsiu);
            delete gsiu;
        };

        int run();
        virtual ~CUpdateTableCommand();

    private:
        Aws::DynamoDB::Model::UpdateTableRequest * m_update_table_request;
    };
}
#endif


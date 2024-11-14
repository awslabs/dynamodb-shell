// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * create_table.hpp
 *
 * command handler for create table
 *
 * Amrith - 2022-08-11
 */

#ifndef __CREATE_TABLE_COMMAND_HPP_DEFINED__
#define __CREATE_TABLE_COMMAND_HPP_DEFINED__

#include "command.hpp"
#include "parser-defs.hpp"
#include "aws-includes.hpp"

namespace ddbsh
{
    class CCreateTableCommand: public CCommand
    {
    public:
        int run();
        CCreateTableCommand (
            std::string table_name,
            bool if_not_exists,
            bool nowait,
            Aws::Vector<Aws::DynamoDB::Model::AttributeDefinition> * attribute_definition,
            Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> * key_schema,
            billing_mode_and_throughput_t * billing_mode_and_throughput,
	    Aws::DynamoDB::Model::WarmThroughput * warm_throughput,
            Aws::Vector<Aws::DynamoDB::Model::GlobalSecondaryIndex> * gsi_list,
            Aws::Vector<Aws::DynamoDB::Model::LocalSecondaryIndex> * lsi_list,
            Aws::DynamoDB::Model::StreamSpecification * stream_specification,
            Aws::DynamoDB::Model::SSESpecification * sse_specification,
            Aws::DynamoDB::Model::TableClass table_class,
            bool deletion_protection,
            Aws::Vector<Aws::DynamoDB::Model::Tag> * tags)
        : m_table_name { table_name },
          m_if_not_exists { if_not_exists },
          m_nowait { nowait },
          m_attribute_definition { attribute_definition },
          m_key_schema { key_schema },
          m_billing_mode_and_throughput { billing_mode_and_throughput },
	  m_warm_throughput { warm_throughput },
          m_gsi_list { gsi_list },
          m_lsi_list { lsi_list },
          m_stream_specification { stream_specification },
          m_sse_specification { sse_specification },
          m_table_class { table_class },
          m_deletion_protection { deletion_protection },
          m_tags { tags } {};

        ~CCreateTableCommand();

    private:
        std::string m_table_name;
        Aws::Vector<Aws::DynamoDB::Model::AttributeDefinition> * m_attribute_definition;
        Aws::Vector<Aws::DynamoDB::Model::KeySchemaElement> * m_key_schema;
        billing_mode_and_throughput_t * m_billing_mode_and_throughput;
        Aws::Vector<Aws::DynamoDB::Model::GlobalSecondaryIndex> * m_gsi_list;
        Aws::Vector<Aws::DynamoDB::Model::LocalSecondaryIndex> * m_lsi_list;
        Aws::DynamoDB::Model::StreamSpecification * m_stream_specification;
        Aws::DynamoDB::Model::SSESpecification * m_sse_specification;
        Aws::DynamoDB::Model::TableClass m_table_class;
        Aws::Vector<Aws::DynamoDB::Model::Tag> * m_tags;
	Aws::DynamoDB::Model::WarmThroughput * m_warm_throughput;
        bool m_if_not_exists;
        bool m_nowait;
        bool m_deletion_protection;
    };
}
#endif

// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * create_table.cpp
 *
 * command handler for create table
 *
 * Amrith - 2022-08-15
 */

#include "ddbsh.hpp"
#include "table_exists.hpp"
#include "logging.hpp"
#include "create_table.hpp"
#include "parser-defs.hpp"
#include "strip_newlines.hpp"

using namespace ddbsh;

int CCreateTableCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    if (m_if_not_exists && table_exists(m_table_name, p_dynamoDBClient))
    {
        logdebug("[%s, %d] create table if not exists. Table exists.\n",
                 __FILENAME__, __LINE__, m_table_name.c_str());
    }
    else
    {
        Aws::DynamoDB::Model::CreateTableRequest request;

        request.SetTableName(m_table_name);

        request.SetAttributeDefinitions(*m_attribute_definition);

        request.SetKeySchema(*m_key_schema);

        if (m_billing_mode_and_throughput)
        {
            request.SetBillingMode(m_billing_mode_and_throughput->mode);

            if (m_billing_mode_and_throughput->mode ==
                Aws::DynamoDB::Model::BillingMode::PROVISIONED)
            {
                request.SetProvisionedThroughput(
                    m_billing_mode_and_throughput->throughput);
            }
	    else if (m_billing_mode_and_throughput->mode ==
                Aws::DynamoDB::Model::BillingMode::PAY_PER_REQUEST)
	    {
		request.SetOnDemandThroughput(m_billing_mode_and_throughput->od_throughput);
	    }
	    else
	    {
		logerror("Error creating table - unknown billing mode.\n");
		return 1;
	    }
        }
        else
        {
            // the default is to choose on-demand. This makes CREATE TABLE simpler.
            request.SetBillingMode(Aws::DynamoDB::Model::BillingMode::PAY_PER_REQUEST);
        }

        if (m_gsi_list)
            request.SetGlobalSecondaryIndexes(*m_gsi_list);

        if (m_lsi_list)
            request.WithLocalSecondaryIndexes(*m_lsi_list);

        if (m_stream_specification)
            request.SetStreamSpecification(*m_stream_specification);

        if (m_sse_specification)
            request.SetSSESpecification(*m_sse_specification);

        request.SetTableClass(m_table_class);

        request.SetDeletionProtectionEnabled(m_deletion_protection);

        if (m_tags)
            request.SetTags(*m_tags);

	if (m_warm_throughput)
	    request.SetWarmThroughput(*m_warm_throughput);

        if (explaining())
        {
            printf("CreateTable(%s)\n", explain_string(request.SerializePayload()).c_str());
        }
        else
        {
            const Aws::DynamoDB::Model::CreateTableOutcome& result =
                p_dynamoDBClient->CreateTable(request);

            if (result.IsSuccess())
            {
                if (!m_nowait)
                    wait_for_table_active(m_table_name, p_dynamoDBClient);

            }
            else
            {
                logerror("Error creating table %s. %s. %s.\n\t%s\n", m_table_name.c_str(),
                         result.GetError().GetExceptionName().c_str(),
                         result.GetError().GetRequestId().c_str(),
                         result.GetError().GetMessage().c_str());
                return 1;
            }
        }
    }

    printf("CREATE\n");
    return 0;
}

CCreateTableCommand::~CCreateTableCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
    delete m_attribute_definition;
    delete m_key_schema;
    FREE(m_billing_mode_and_throughput);
    delete m_gsi_list;
    delete m_lsi_list;
    delete m_stream_specification;
    delete m_sse_specification;
    delete m_tags;
}

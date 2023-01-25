// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * drop_table.cpp
 *
 * command handler for drop table
 *
 * Amrith - 2022-08-12
 */

#include "ddbsh.hpp"
#include "drop_table.hpp"
#include "table_exists.hpp"
#include "logging.hpp"
#include "aws-includes.hpp"
#include "strip_newlines.hpp"

using namespace ddbsh;

int CDropTableCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);

    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    if (!m_exists || (m_exists && table_exists(m_table, p_dynamoDBClient)))
    {
        Aws::DynamoDB::Model::DeleteTableRequest dtr;
        dtr.SetTableName(m_table);

        if (explaining())
        {
            printf("DeleteTable(%s)\n", explain_string(dtr.SerializePayload()).c_str());
        }
        else
        {
            const Aws::DynamoDB::Model::DeleteTableOutcome& dto = p_dynamoDBClient->DeleteTable(dtr);

            if (!dto.IsSuccess())
            {
                logerror ("Drop Table Failed. %s. %s.\n\t%s\n",
                          dto.GetError().GetExceptionName().c_str(),
                          dto.GetError().GetRequestId().c_str(),
                          dto.GetError().GetMessage().c_str());
                return 1;
            }
            else
            {
                if (!m_nowait)
                    wait_for_table_gone(m_table, p_dynamoDBClient);

                printf("DROP\n");
            }
        }
    }

    return 0;
}

CDropTableCommand::CDropTableCommand(std::string table, bool exists, bool nowait)
{
    m_table = table;
    m_exists = exists;
    m_nowait = nowait;
}

CDropTableCommand::~CDropTableCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
}


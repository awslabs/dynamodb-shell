// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * ddbsh.cpp
 *
 * The main class for ddbsh, instantiated from main.cpp
 *
 * Amrith. 2022-08-10
 *
 */

#include "ddbsh.hpp"
#include "command.hpp"
#include <parser-defs.hpp>
#include <string>
#include <readline/readline.h>
#include <readline/history.h>
#include <stdlib.h>
#include "logging.hpp"
#include <stdio.h>
#include "allocation.hpp"

#include <iostream>

#include "aws-includes.hpp"

using namespace ddbsh;
#include "parser.hpp"
#include "ddbsh_ddb_client_config.hpp"

extern int yylineno;
extern "C" int yylex(void);
extern int yyparse(CCommand **);
extern "C" FILE* yyin;

CDDBSh::CDDBSh()
{
    m_quit = false;

    if (std::getenv("DDBSH_CONFIG_FILE") != nullptr)
        m_config_file = std::getenv("DDBSH_CONFIG_FILE");
    else
    {
        m_config_file = getenv("HOME");
        m_config_file += "/.ddbsh_config";
    }

    if (std::getenv("AWS_DEFAULT_REGION") != nullptr)
        m_region = std::getenv("AWS_DEFAULT_REGION");
    else
        m_region = "us-east-1";

    if (std::getenv("DDBSH_ENDPOINT_OVERRIDE") != nullptr)
        m_endpoint = std::getenv("DDBSH_ENDPOINT_OVERRIDE");
    else
        m_endpoint = "";

    // load history file
    m_history_file = getenv("HOME");
    m_history_file += "/.ddbsh_history";
    read_history(m_history_file.c_str());

    p_dynamoDBClient = NULL;
    m_reconnect = true;

    Aws::InitAPI(m_options);

    // initializer
    st_uniq = 0;
}

CDDBSh::~CDDBSh()
{
    write_history(m_history_file.c_str());

    delete p_dynamoDBClient;
    Aws::ShutdownAPI(m_options);
}

CDDBSh * CDDBSh::getInstance()
{
    static CDDBSh me;
    return &me;
}

void CDDBSh::quit()
{
    m_quit = true;
}

int CDDBSh::run()
{
    CCommand * pCommand = NULL;
    int retval = 0;

    // this is used only in readline anyway.
    yyin = stdin;
    m_quit = false;

    do
    {
        int allocs, frees;

        yylineno = 1;

        logdebug("[%s, %d] At the top of run().\n", __FILENAME__, __LINE__);

        if (m_reconnect)
            reconnect();

        yyparse(&pCommand);

        logdebug("[%s, %d] Done with yyparse(). pCommand = %p\n",
                 __FILENAME__, __LINE__, pCommand);

        if (pCommand)
        {
            logdebug("[%s, %d] Will invoke run().\n", __FILENAME__, __LINE__);
            retval = pCommand->run();
            logdebug("[%s, %d] Done with run() retval = %d.\n", __FILENAME__, __LINE__, retval);
            delete pCommand;
            pCommand = NULL;
        }

#ifdef TEST_HARNESS
        get_allocations(&allocs, &frees);
        if (allocs != frees)
        {
            logdebug("[%s, %d] memory leak - allocations = %d, frees = %d\n",
                     __FILENAME__, __LINE__, allocs, frees);
            show_allocations();
            exit(1);
        }
        else
        {
            logdebug("[%s, %d] no memory leak - allocations = %d, frees = %d\n",
                     __FILENAME__, __LINE__, allocs, frees);
        }
#endif
    } while (!feof(yyin) && !m_quit && (!retval || !abort_on_error()));

    return retval;
}

void CDDBSh::setRegionAndEndpoint(std::string region, std::string endpoint)
{
    m_region = region;
    m_endpoint = endpoint;

    m_reconnect = true;
}

std::string CDDBSh::getRegion()
{
    if (m_endpoint.empty())
        return m_region;
    else
        return m_region + " (*)";
}

void CDDBSh::reconnect()
{
    logdebug("[%s, %d] Will attempt reconnect.\n", __FILENAME__, __LINE__);
    delete p_dynamoDBClient;

    CDDBShDDBClientConfig clientConfig(m_region, m_endpoint);
    p_dynamoDBClient = new Aws::DynamoDB::DynamoDBClient(clientConfig);

    if(checkConnection())
        m_reconnect = false;
    else
        logdebug("[%s, %d] The reconnect was successful.\n",
                 __FILENAME__, __LINE__);

    return;
}

bool CDDBSh::checkConnection()
{
    Aws::DynamoDB::Model::ListTablesRequest listTablesRequest;
    listTablesRequest.SetLimit(2);

    const Aws::DynamoDB::Model::ListTablesOutcome& lto = p_dynamoDBClient->ListTables(listTablesRequest);
    if (!lto.IsSuccess())
    {
        logerror("[%s, %d], Connection Failed. %s. %s. \n\t%s\n",
                 __FILENAME__, __LINE__,
                 lto.GetError().GetExceptionName().c_str(),
                 lto.GetError().GetRequestId().c_str(),
                 lto.GetError().GetMessage().c_str());
        return false;
    }

    return true;
}

Aws::DynamoDB::DynamoDBClient * CDDBSh::getClient()
{
    return p_dynamoDBClient;
}

#define DDBSH_UNIQUE_PREFIX_LEN   (3)
#define DDBSH_UNIQUE_PREFIX_CHARS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
std::string CDDBSh::st_unique_prefix()
{
    int val = this->st_uniq++, b;
    std::string prefixchars = DDBSH_UNIQUE_PREFIX_CHARS;
    std::string prefix;
    for (int ix = 0; ix < DDBSH_UNIQUE_PREFIX_LEN; ix ++)
    {
        prefix += prefixchars[val % prefixchars.length()];
        val /= prefixchars.length();
    }

    return std::string(prefix);
}

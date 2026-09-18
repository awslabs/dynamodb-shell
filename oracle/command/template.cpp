// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * template.cpp
 *
 * command handler for template
 *
 * Amrith - 2022-08-15
 */

#include "ddbsh.hpp"
#include "template.hpp"
#include "logging.hpp"

#include "aws-includes.hpp"

using namespace ddbsh;

int CTemplateCommand::run()
{
    logdebug("[%s, %d] entering run()\n", __FILENAME__, __LINE__);
    Aws::DynamoDB::DynamoDBClient * p_dynamoDBClient;

    p_dynamoDBClient = CDDBSh::getInstance()->getClient();

    return 0;
}

CTemplateCommand::~CTemplateCommand()
{
    logdebug("[%s, %d] In destructor.\n", __FILENAME__, __LINE__);
}


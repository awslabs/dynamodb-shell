// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * quit.cpp
 *
 * command handler for quit
 *
 * Amrith - 2022-08-10
 */

#include "ddbsh.hpp"
#include "quit.hpp"

using namespace ddbsh;

int CQuitCommand::run()
{
    printf("QUIT\n");

    if (explaining())
    {
        printf("There is no good reason to quit. It is inexplicable.\n");
    }

    CDDBSh::getInstance()->quit();
    return 0;
}

CQuitCommand::~CQuitCommand()
{
}

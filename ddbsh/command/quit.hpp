// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * quit.hpp
 *
 * command handler for quit
 *
 * Amrith - 2022-08-10
 */

#ifndef __QUIT_HPP_DEFINED__
#define __QUIT_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{

    class CQuitCommand: public CCommand
    {
    public:
        int run();
        virtual ~CQuitCommand();
    };

}
#endif

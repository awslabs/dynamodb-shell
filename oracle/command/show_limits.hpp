// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * show_limits.hpp
 *
 * command handler for show_limits command
 *
 * Amrith - 2022-08-15
 */

#ifndef __SHOW_LIMITS_COMMAND_HPP_DEFINED__
#define __SHOW_LIMITS_COMMAND_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{
    class CShowLimitsCommand: public CCommand
    {
    public:
        CShowLimitsCommand() {};
        int run();
        virtual ~CShowLimitsCommand();
    };
}
#endif

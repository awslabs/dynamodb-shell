// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * template.hpp
 *
 * command handler for template command
 *
 * Amrith - 2022-08-15
 */

#ifndef __TEMPLATE_COMMAND_HPP_DEFINED__
#define __TEMPLATE_COMMAND_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{
    class CTemplateCommand: public CCommand
    {
    public:
        CTemplateCommand();
        int run();
        virtual ~CTemplateCommand();
    };
}
#endif

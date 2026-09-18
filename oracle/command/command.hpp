// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * command.hpp
 *
 * The base class of a command.
 *
 * Amrith Kumar. 2022-08-10
 */

#ifndef __COMMAND_HPP_DEFINED__
#define __COMMAND_HPP_DEFINED__

#include "aws-includes.hpp"

#include <iostream>

namespace ddbsh
{
    class CCommand
    {
    public:
        CCommand() {
            m_explain = false;
        };

        virtual int run() { return 0; };

        virtual ~CCommand() { };

        virtual void explain() final {
            m_explain = true;
        };

        virtual Aws::DynamoDB::Model::TransactGetItem * txget() {
            // override where required
            return NULL;
        };

        virtual Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * txwrite() {
            // override where required
            return NULL;
        };

    protected:
        virtual bool explaining() final {
            return m_explain;
        };

    private:
        bool m_explain;
    };
};

#endif

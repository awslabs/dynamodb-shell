// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * select.hpp
 *
 * the class definition for CSelect
 *
 * Amrith - 2022-10-07
 */

#ifndef __SELECT_COMMAND_HPP_DEFINED__
#define __SELECT_COMMAND_HPP_DEFINED__

#include "aws-includes.hpp"
#include "parser-defs.hpp"
#include "logging.hpp"
#include "command.hpp"
#include "symbol_table.hpp"
#include "select_helper.hpp"
#include "ratelimit.hpp"

namespace ddbsh
{

    class CSelectCommand : public CCommand
    {
    public:
        CSelectCommand(CRateLimit * ratelimit, bool consistent, Aws::Vector<Aws::String> * projection,
                       Aws::Vector<Aws::String> * table, CWhere * where, Aws::DynamoDB::Model::ReturnConsumedCapacity returns) {
            m_exists = false;
            m_helper.setup(consistent, projection, table, where, returns, ratelimit ? true : false);
            m_ratelimit = ratelimit;
            m_returns = returns;
        };

        ~CSelectCommand() {
            delete m_ratelimit;
        };

        virtual int run();

        virtual Aws::DynamoDB::Model::TransactGetItem * txget();
        Aws::Vector<Aws::DynamoDB::Model::TransactWriteItem> * txwrite();

        virtual void exists() final {
            m_exists = true;
        };

    private:
        CSelectHelper m_helper;
        CRateLimit * m_ratelimit;
        Aws::DynamoDB::Model::ReturnConsumedCapacity m_returns;
        bool m_exists;

        int do_scan();
        int do_query();
        int do_getitem();

        void display_consumed_capacity(const Aws::DynamoDB::Model::ConsumedCapacity cc);
    };

}
#endif

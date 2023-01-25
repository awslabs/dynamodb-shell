// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * transact.hpp
 *
 * The base class of a transact command. This is intended to be
 * pure/abstract/base class which read and write transactions can
 * inherit from
 *
 * Amrith Kumar. 2022-12-10
 */

#ifndef __TRANSACT_HPP_DEFINED__
#define __TRANSACT_HPP_DEFINED__

#include "command.hpp"

namespace ddbsh
{
    class CTransactCommand : public CCommand
    {
    public:
        CTransactCommand() {
            logdebug("[%s, %d] In %s.\n", __FILENAME__, __LINE__, __FUNCTION__);
            m_committed = false;
            m_abort = false;
        };

        virtual void append(CCommand * pCommand) = 0;
        virtual void commit() = 0;
        virtual void rollback() = 0;
        virtual void abort() final {
            m_abort = true;
        };

        virtual bool isaborted() final {
            return m_abort;
        };

    protected:
        bool m_committed;
        bool m_abort;
    };
}
#endif

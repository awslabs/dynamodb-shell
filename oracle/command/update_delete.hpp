// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * update_delete.hpp
 *
 * derived class from CCommand to account for the peculiarities of update and delete.
 *
 * Amrith - 2022-11-23
 */

#ifndef __UPDATE_DELETE_HPP_DEFINED__
#define __UPDATE_DELETE_HPP_DEFINED__

namespace ddbsh
{
    class CUpdateDeleteCommand: public CCommand
    {
    public:
        CUpdateDeleteCommand() {
            m_nread = 0;
            m_nmodified = 0;
            m_nccf = 0;
        };

        void read() {
            m_nread ++;
        };

        void modified() {
            m_nmodified ++;
        };

        void ccf() {
            m_nccf ++;
        };

        unsigned int nread() {
            return m_nread;
        };

        unsigned int nmodified() {
            return m_nmodified;
        };

        unsigned int nccf() {
            return m_nccf;
        };

    private:
        unsigned int m_nread;
        unsigned int m_nmodified;
        unsigned int m_nccf;
    };
}
#endif

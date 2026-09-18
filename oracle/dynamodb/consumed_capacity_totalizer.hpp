// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * consumed_capacity_totalizer.hpp
 *
 * wrapper on ConsumedCapacity to account for paginated results.
 *
 * -Amrith - 2022-11-05
 */

#ifndef __CONSUMED_CAPACITY_TOTALIZER_HPP_DEFINED__
#define __CONSUMED_CAPACITY_TOTALIZER_HPP_DEFINED__

#include "aws-includes.hpp"
#include "logging.hpp"

class CConsumedCapacityTotalizer
{
public:
    void add(Aws::DynamoDB::Model::ConsumedCapacity cc) {
        logdebug("[%s, %d] cc (%d, %d, %d)\n", __FILENAME__, __LINE__, (int) cc.GetCapacityUnits(),
                 (int) cc.GetReadCapacityUnits(), (int) cc.GetWriteCapacityUnits());

        if (m_capacity.GetTableName().empty()) {
            m_capacity = cc;
        } else {
            assert(cc.GetTableName() == m_capacity.GetTableName());

            m_capacity.SetCapacityUnits(m_capacity.GetCapacityUnits() + cc.GetCapacityUnits());
            m_capacity.SetReadCapacityUnits(m_capacity.GetReadCapacityUnits() + cc.GetReadCapacityUnits());
            m_capacity.SetWriteCapacityUnits(m_capacity.GetWriteCapacityUnits() + cc.GetWriteCapacityUnits());
        }
    };

    Aws::DynamoDB::Model::ConsumedCapacity get() {
        return m_capacity;
    };

private:
    Aws::DynamoDB::Model::ConsumedCapacity m_capacity;
};

#endif

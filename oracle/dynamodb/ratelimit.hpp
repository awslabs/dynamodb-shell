// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * ratelimit.hpp
 *
 * A simple token bucket rate limiter implementation for dynamodb
 * shell. Rates are measured in read and write requests per second.
 *
 * Amrith - 2023-04-22
 */

#ifndef __RATELIMIT_HPP_DEFINED__
#define __RATELIMIT_HPP_DEFINED__

#include <sys/time.h>
#define CRATELIMIT_SEC_TO_USEC (1000000.0)

#include "aws-includes.hpp" // yes - this is excessive, isn't it?
#include "logging.hpp"

namespace ddbsh
{
    class CRateLimit
    {
    public:
        CRateLimit (unsigned long rcu, unsigned long wcu) {
            m_rcu = rcu;
            m_wcu = wcu;
            gettimeofday(&m_last_time, NULL);
            m_read_tokens = 0.0;
            m_write_tokens = 0.0;
            __log_ratelimit("CRateLimit");
        };

        void consume_reads(Aws::DynamoDB::Model::ConsumedCapacity cc) {
            logdebug("[%s, %d] cc (%d, %d, %d)\n", __FILENAME__, __LINE__, (int) cc.GetCapacityUnits(),
                     (int) cc.GetReadCapacityUnits(), (int) cc.GetWriteCapacityUnits());

            m_read_tokens -= cc.GetCapacityUnits();
        };

        void consume_writes(Aws::DynamoDB::Model::ConsumedCapacity cc) {
            logdebug("[%s, %d] cc (%d, %d, %d)\n", __FILENAME__, __LINE__, (int) cc.GetCapacityUnits(),
                     (int) cc.GetReadCapacityUnits(), (int) cc.GetWriteCapacityUnits());

            m_write_tokens -= cc.GetCapacityUnits();
        };

        void readlimit() {
            refresh();

            // a read limit of 0 indicates that there is no read
            // limiting. This also prevents a later division by zero
            // error. Also return immediately if we have read tokens.
            if (m_rcu == 0 || m_read_tokens >= 0)
                return;

            do
            {
                if (m_read_tokens < 0) {
                    double wait_time = ((- m_read_tokens)/m_rcu) * CRATELIMIT_SEC_TO_USEC;
                    logdebug("[%s, %d] will sleep %d us\n", __FILENAME__, __LINE__, (int) wait_time);
                    usleep((int) wait_time);
                }

                refresh();
            } while(m_read_tokens < 0);
        };

        void writelimit() {
            refresh();

            // a write limit of 0 indicates that there is no write
            // limiting. This also prevents a later division by zero
            // error. Also return immediately if we have write tokens.
            if (m_wcu == 0 || m_write_tokens >= 0)
                return;

            do
            {
                if (m_write_tokens < 0) {
                    double wait_time = ((- m_write_tokens)/m_wcu) * CRATELIMIT_SEC_TO_USEC;
                    logdebug("[%s, %d] will sleep %d us\n", __FILENAME__, __LINE__, (int) wait_time);
                    usleep((int) wait_time);
                }

                refresh();
            } while(m_write_tokens < 0);
        };

    private:
        // the rate at which read and write tokens are to be refilled,
        // measured as tokens per second.
        double m_rcu;
        double m_wcu;

        // the last time that the token counters were refreshed.
        struct timeval m_last_time;
        double m_read_tokens;
        double m_write_tokens;

        void __log_ratelimit (std::string tag) {
            logdebug("[%s, %d] %s ratelimit (rr = %d/s, wr = %d/s, last = %d.%07d, rt = %.06lf, wt = %.06lf\n",
                     __FILENAME__, __LINE__, tag.c_str(), (int) m_rcu, (int) m_wcu, m_last_time.tv_sec, m_last_time.tv_usec,
                     m_read_tokens, m_write_tokens);
        };

        void refresh() {
            struct timeval now, delta;
            __log_ratelimit("before refresh");
            gettimeofday(&now, NULL);
            timersub(&now, &m_last_time, &delta);

            double elapsed = delta.tv_sec + (delta.tv_usec / CRATELIMIT_SEC_TO_USEC);

            if ((m_read_tokens += elapsed * m_rcu) > m_rcu)
                m_read_tokens = m_rcu;

            if ((m_write_tokens += elapsed * m_wcu) > m_wcu)
                m_write_tokens = m_wcu;

            m_last_time = now;
            __log_ratelimit("after refresh");
        };
    };
};
#endif

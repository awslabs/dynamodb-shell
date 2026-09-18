// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
/*
 * table_exists.cpp
 *
 * helper to check if a table exists, and to wait for a table to reach a
 * desired control-plane state.
 *
 * Amrith - 2022-08-12
 */

#include <unistd.h>
#include <chrono>
#include <string>
#include <thread>
#include "table_exists.hpp"
#include "logging.hpp"

#include "aws-includes.hpp"

bool table_exists (std::string name,
                   Aws::DynamoDB::DynamoDBClient * pClient)
{
    Aws::DynamoDB::Model::DescribeTableRequest dtr;
    dtr.SetTableName(name);
    const Aws::DynamoDB::Model::DescribeTableOutcome& result =  \
        pClient->DescribeTable(dtr);

    if (result.IsSuccess())
    {
        logdebug("[%s, %d] Found table %s to exist.\n", __FILENAME__, __LINE__,
                 name.c_str());
        Aws::DynamoDB::Model::DescribeTableResult dtresult = result.GetResult();
        Aws::DynamoDB::Model::TableDescription td = dtresult.GetTable();

        logdebug("[%s, %d] status = %s\n", __FILENAME__, __LINE__,
                 Aws::DynamoDB::Model::TableStatusMapper::GetNameForTableStatus(td.GetTableStatus()).c_str());
        return true;
    }
    else
    {
        if (!(result.GetError().GetErrorType() ==
              Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND))
            logerror("[%s, %d] %s. %s. %s\n", __FILENAME__, __LINE__,
                     result.GetError().GetExceptionName().c_str(),
                     result.GetError().GetRequestId().c_str(),
                     result.GetError().GetMessage().c_str());

        return false;
    }
}

// ---------------------------------------------------------------------------
// Bounded, error-aware waiters for control-plane state transitions.
//
// DescribeTable is eventually consistent: per the API docs a DescribeTable
// issued immediately after CreateTable "might return a ResourceNotFoundException"
// until the table metadata propagates. It can also fail transiently
// (InternalServerError, throttling). Each poll is therefore classified as:
//   - active         : DescribeTable succeeded and TableStatus == ACTIVE
//   - transitioning  : DescribeTable succeeded, some other status (still exists)
//   - gone           : ResourceNotFoundException (table absent)
//   - transient_error: some other error the SDK marks retryable
//   - terminal_error : a non-retryable error (e.g. AccessDenied, Validation)
// The waiters block indefinitely while the table is legitimately transitioning
// (or a poll fails transiently) -- matching real control-plane semantics, where
// a create/delete completes when the service says so, not on a client clock.
// They still return promptly (false, reason logged) on a terminal condition: a
// non-retryable error, or a state that cannot progress to the target. Both
// return true on reaching the desired state.
// ---------------------------------------------------------------------------

enum class poll_state { active, transitioning, gone, transient_error, terminal_error };

static poll_state poll_table_once(const std::string & name,
                                  Aws::DynamoDB::DynamoDBClient * pClient,
                                  Aws::DynamoDB::Model::TableStatus & status_out,
                                  std::string & error_out)
{
    Aws::DynamoDB::Model::DescribeTableRequest dtr;
    dtr.SetTableName(name);
    const Aws::DynamoDB::Model::DescribeTableOutcome result = pClient->DescribeTable(dtr);

    if (result.IsSuccess())
    {
        status_out = result.GetResult().GetTable().GetTableStatus();
        return (status_out == Aws::DynamoDB::Model::TableStatus::ACTIVE)
            ? poll_state::active : poll_state::transitioning;
    }

    const Aws::Client::AWSError<Aws::DynamoDB::DynamoDBErrors> & err = result.GetError();
    if (err.GetErrorType() == Aws::DynamoDB::DynamoDBErrors::RESOURCE_NOT_FOUND)
        return poll_state::gone;

    error_out = std::string(err.GetExceptionName().c_str()) + ". " + err.GetMessage().c_str();
    return err.ShouldRetry() ? poll_state::transient_error : poll_state::terminal_error;
}

// Sleep for the current interval, then return the next one (capped 100ms->2s).
static std::chrono::milliseconds backoff_sleep(std::chrono::milliseconds current)
{
    std::this_thread::sleep_for(current);
    std::chrono::milliseconds next = current * 2;
    return (next < std::chrono::milliseconds(2000)) ? next : std::chrono::milliseconds(2000);
}

// Only CREATING/UPDATING progress toward ACTIVE. ARCHIVING/ARCHIVED/
// INACCESSIBLE_ENCRYPTION_CREDENTIALS are states that waiting will not resolve.
static bool progresses_to_active(Aws::DynamoDB::Model::TableStatus s)
{
    return s == Aws::DynamoDB::Model::TableStatus::CREATING
        || s == Aws::DynamoDB::Model::TableStatus::UPDATING;
}

bool wait_for_table_active(std::string name, Aws::DynamoDB::DynamoDBClient * pClient)
{
    std::chrono::milliseconds interval(100);

    while (true)
    {
        Aws::DynamoDB::Model::TableStatus status = Aws::DynamoDB::Model::TableStatus::NOT_SET;
        std::string errmsg;

        switch (poll_table_once(name, pClient, status, errmsg))
        {
        case poll_state::active:
            logdebug("[%s, %d] table %s is ACTIVE.\n", __FILENAME__, __LINE__, name.c_str());
            return true;

        case poll_state::transitioning:
            if (!progresses_to_active(status))
            {
                logerror("[%s, %d] Table %s is in state %s and will not become ACTIVE.\n",
                         __FILENAME__, __LINE__, name.c_str(),
                         Aws::DynamoDB::Model::TableStatusMapper::GetNameForTableStatus(status).c_str());
                return false;
            }
            break; // CREATING/UPDATING: keep waiting

        case poll_state::gone:
            // Eventually-consistent DescribeTable can briefly report the table
            // missing right after CreateTable; keep polling until it appears.
            logdebug("[%s, %d] table %s not yet visible, waiting.\n", __FILENAME__, __LINE__, name.c_str());
            break;

        case poll_state::transient_error:
            logdebug("[%s, %d] transient error waiting for %s to become ACTIVE. %s\n",
                     __FILENAME__, __LINE__, name.c_str(), errmsg.c_str());
            break; // retryable: keep waiting

        case poll_state::terminal_error:
            logerror("[%s, %d] Failed waiting for table %s to become ACTIVE. %s\n",
                     __FILENAME__, __LINE__, name.c_str(), errmsg.c_str());
            return false;
        }

        interval = backoff_sleep(interval);
    }
}

bool wait_for_table_gone(std::string name, Aws::DynamoDB::DynamoDBClient * pClient)
{
    std::chrono::milliseconds interval(100);

    while (true)
    {
        Aws::DynamoDB::Model::TableStatus status = Aws::DynamoDB::Model::TableStatus::NOT_SET;
        std::string errmsg;

        switch (poll_table_once(name, pClient, status, errmsg))
        {
        case poll_state::gone:
            logdebug("[%s, %d] table %s is gone.\n", __FILENAME__, __LINE__, name.c_str());
            return true;

        case poll_state::active:
        case poll_state::transitioning:
            // still exists in some state (including DELETING): keep waiting
            break;

        case poll_state::transient_error:
            logdebug("[%s, %d] transient error waiting for %s to be deleted. %s\n",
                     __FILENAME__, __LINE__, name.c_str(), errmsg.c_str());
            break; // retryable: keep waiting

        case poll_state::terminal_error:
            logerror("[%s, %d] Failed waiting for table %s to be deleted. %s\n",
                     __FILENAME__, __LINE__, name.c_str(), errmsg.c_str());
            return false;
        }

        interval = backoff_sleep(interval);
    }
}

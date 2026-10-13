// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// create_table_executor.hpp — execute CREATE TABLE against the IDDBClient.
//
// PRIVATE engine header (not installed). SDK-free port of
// CCreateTableCommand::run() (arch §7). It copies the parsed table definition
// into a CreateTableIn request, issues create_table, and (unless NOWAIT) waits
// for the table to become ACTIVE. IF NOT EXISTS first probes the table via
// describe_table and, when it already exists, succeeds silently WITHOUT a
// create call while still emitting the CREATE status token — exactly as the
// original (which, under `if_not_exists && table_exists`, skips the request and
// still prints "CREATE").
//
// Billing default (arch §8): a nullopt `billing` means "no billing clause"; the
// service default is on-demand (PAY_PER_REQUEST). The executor forwards the
// nullopt to the request and the CLI adapter applies PAY_PER_REQUEST, matching
// where the original applied the default (create_table.cpp).
//
// EXPLAIN of a CREATE is NOT handled here (it renders the serialized
// CreateTable request instead and must not contact the service — W6); the
// caller routes an explaining CREATE elsewhere.
//
// D12: never aborts; a service or waiter failure is returned as an Error.

#ifndef DDBSH_SRC_EXECUTE_CREATE_TABLE_EXECUTOR_HPP
#define DDBSH_SRC_EXECUTE_CREATE_TABLE_EXECUTOR_HPP

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Execute a (non-EXPLAIN) CREATE TABLE. Returns StatusOutcome{"CREATE"} on a
/// successful create (or when IF NOT EXISTS matched an existing table), or an
/// Error on a create / waiter failure.
Result<ExecOutcome> execute_create_table(const CreateTableCmd& cmd, IDDBClient& client);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_CREATE_TABLE_EXECUTOR_HPP

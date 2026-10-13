// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// get_key_schema.hpp — resolve a table/index partition+sort key.
//
// PRIVATE engine header (not installed). SDK-free port of the original
// dynamodb/get_key_schema.cpp. The planner needs the key schema to decide
// GetItem/Query/Scan (arch §8); the original fetched it with DescribeTable and
// this does the same through the injected IDDBClient (arch §10). When a
// `.index` is named, LSIs are searched before GSIs, exactly as the original.
//
// D12: never aborts — a missing table/index/key is returned as an Error.

#ifndef DDBSH_SRC_PLANNER_GET_KEY_SCHEMA_HPP
#define DDBSH_SRC_PLANNER_GET_KEY_SCHEMA_HPP

#include <string>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"

namespace ddbsh::engine {

/// The resolved partition (pk) and sort (rk, "" if none) key attribute names.
struct ResolvedKeySchema {
    std::string pk;
    std::string rk;
};

/// Resolve the key schema for `table` (when `index` is empty) or for the named
/// secondary index on that table (LSIs searched before GSIs). Calls
/// client.describe_table. Propagates the client's error if the table cannot be
/// described; returns a no_such_table error if a named index is not found.
Result<ResolvedKeySchema> get_key_schema(IDDBClient& client, const std::string& table,
                                         const std::string& index);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_PLANNER_GET_KEY_SCHEMA_HPP

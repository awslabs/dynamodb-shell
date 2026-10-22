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
#include <vector>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"

namespace ddbsh::engine {

/// The resolved partition (pk) and sort (rk) key attribute names, in composite
/// order. Base tables and LSIs always yield |pk|==1 and |rk|<=1; a composite
/// GSI may yield up to 4 pk and 4 rk attributes (phase 4a).
struct ResolvedKeySchema {
    std::vector<std::string> pk;
    std::vector<std::string> rk;
};

/// Resolve the key schema for `table` (when `index` is empty) or for the named
/// secondary index on that table (LSIs searched before GSIs). Calls
/// client.describe_table. Propagates the client's error if the table cannot be
/// described; returns a no_such_table error if a named index is not found.
Result<ResolvedKeySchema> get_key_schema(IDDBClient& client, const std::string& table,
                                         const std::string& index);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_PLANNER_GET_KEY_SCHEMA_HPP

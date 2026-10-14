// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// select_planner.hpp — the SELECT -> GetItem/Query/Scan decision + request
// builder.
//
// PRIVATE engine header (not installed). SDK-free port of the original
// dynamodb/select_helper.cpp + the CSelectCommand::run() dispatch. Given a
// parsed SelectCmd and the injected IDDBClient (used only to resolve the key
// schema, arch §10), it decides the cheapest access pattern and builds the
// matching SDK-free request struct, aliasing every expression attribute/value
// through ONE SymbolTable so EXPLAIN output is byte-identical to the original
// (arch §6, §8). It does not execute — W5 drives the chosen request against
// the client and W6 renders the result/EXPLAIN.
//
// Decision order (exactly CSelectCommand::run()):
//   can_getitem -> GetItem ; else can_query -> Query ; else Scan.
//
// D12: never aborts; a key-schema resolution failure is returned as an Error.

#ifndef DDBSH_SRC_PLANNER_SELECT_PLANNER_HPP
#define DDBSH_SRC_PLANNER_SELECT_PLANNER_HPP

#include <string>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Which API the planner chose for a SELECT.
enum class SelectApi { get_item, query, scan };

/// The planned request. Exactly one of the three request members is populated,
/// selected by `api`. The populated member is ready to hand to the matching
/// IDDBClient op (W5) or to render as EXPLAIN (W6).
struct SelectPlan {
    SelectApi api = SelectApi::scan;
    GetItemIn get_item;
    QueryIn query;
    ScanIn scan;
};

/// Plan a SELECT. `alias_prefix` seeds the single SymbolTable (one per SELECT,
/// matching the original's one-CSymbolTable-per-request behavior); callers
/// obtain it from Session::next_alias_prefix(). Resolves the key schema via
/// client.describe_table (arch §10).
Result<SelectPlan> plan_select(const SelectCmd& cmd, IDDBClient& client,
                               const std::string& alias_prefix);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_PLANNER_SELECT_PLANNER_HPP

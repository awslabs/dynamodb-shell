// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// select_executor.hpp — drive a planned SELECT against the IDDBClient.
//
// PRIVATE engine header (not installed). SDK-free port of CSelectCommand::run()
// and its do_getitem/do_query/do_scan paginated loops (arch §7/§8/§10). Given a
// parsed SelectCmd, it plans the access pattern (planner), issues the matching
// IDDBClient op, follows Query/Scan pagination via the LastEvaluatedKey cursor,
// totalizes consumed capacity across pages, and returns a SelectOutcome (DATA,
// never bytes — the Renderer formats it, P2a-03).
//
// EXPLAIN of a SELECT is NOT handled here: EXPLAIN must not contact the service
// and renders the serialized request(s) instead, which lands with the EXPLAIN
// Renderer slice (W6). The caller routes an explaining SELECT elsewhere.
//
// D12: never aborts; a key-schema or service failure is returned as an Error.

#ifndef DDBSH_SRC_EXECUTE_SELECT_EXECUTOR_HPP
#define DDBSH_SRC_EXECUTE_SELECT_EXECUTOR_HPP

#include <string>

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Execute a (non-EXPLAIN) SELECT. `alias_prefix` seeds the single SymbolTable
/// the planner uses (one per statement; obtained from
/// Session::next_alias_prefix()). Returns a SelectOutcome.
Result<ExecOutcome> execute_select(const SelectCmd& cmd, IDDBClient& client,
                                   const std::string& alias_prefix);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_SELECT_EXECUTOR_HPP

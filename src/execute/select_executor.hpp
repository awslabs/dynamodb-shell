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
// EXPLAIN of a SELECT is handled by execute_select_explain: it plans the same
// access pattern but, instead of issuing the op, serializes the chosen request
// (via IDDBClient::explain_request) into an ExplainOutcome and does NOT contact
// the service for data (it still resolves the key schema to plan — arch §8).
//
// D12: never aborts; a key-schema or service failure is returned as an Error.

#ifndef DDBSH_SRC_EXECUTE_SELECT_EXECUTOR_HPP
#define DDBSH_SRC_EXECUTE_SELECT_EXECUTOR_HPP

#include <string>

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/outcome_sink.hpp"
#include "ddbsh/result.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Execute a (non-EXPLAIN) SELECT, BUFFERING the whole result into one
/// SelectOutcome (GetItem/Query/Scan) or VectorSearchOutcome. `alias_prefix`
/// seeds the single SymbolTable the planner uses (one per statement; obtained
/// from Session::next_alias_prefix()). This is the buffering contract the
/// Session::execute(stmt) overload and the native unit tests rely on; the
/// streaming CLI path uses execute_select_streaming instead (D-P5-1).
Result<ExecOutcome> execute_select(const SelectCmd& cmd, IDDBClient& client,
                                   const std::string& alias_prefix);

/// Execute a (non-EXPLAIN) SELECT, STREAMING the result to `sink` with bounded
/// memory (D-P5-1 / D2). For Query/Scan it drives a SelectRowCursor and emits
/// one SelectOutcome{page_items, nullopt} fragment per page, then — iff the
/// user asked for capacity (RETURN CONSUMED CAPACITY) and the service returned
/// any — a final SelectOutcome{{}, capacity} fragment. GetItem (a single item)
/// and SearchVectors (a single bounded request) emit their one outcome directly
/// (no per-page split). The concatenation of the emitted fragments renders
/// byte-identically to the single buffered SelectOutcome (the Renderer emits
/// item lines then, iff present, the one `SELECT (...)` line — arch §9), so the
/// golden output is unchanged. Returns success, or the Error that stopped the
/// paging (fragments already emitted are the partial output).
Result<void> execute_select_streaming(const SelectCmd& cmd, IDDBClient& client,
                                      const std::string& alias_prefix, OutcomeSink& sink);

/// EXPLAIN a SELECT: plan the access pattern (same decision as execute_select)
/// and serialize the chosen GetItem/Query/Scan request into an ExplainOutcome
/// (one block). Returns the ExplainOutcome, or an Error if planning/serializing
/// fails.
Result<ExecOutcome> execute_select_explain(const SelectCmd& cmd, IDDBClient& client,
                                           const std::string& alias_prefix);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_SELECT_EXECUTOR_HPP

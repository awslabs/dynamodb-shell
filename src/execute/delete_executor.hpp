// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// delete_executor.hpp — execute DELETE against the IDDBClient.
//
// PRIVATE engine header (not installed). SDK-free port of CDeleteCommand
// (command/delete.cpp, arch §7/§8). The dispatch parallels UPDATE:
//   * full base-table key (can_delete_item) -> ONE DeleteItem with the
//     condition (read=0);
//   * else -> Where::plan_query(key schema) chooses the enumeration access
//     pattern: a QueryKeyPlan (full-PK equality + gap-free key-legal sort-key
//     prefix) -> Query, otherwise -> Scan. A `table.index` target routes the
//     read through the (possibly composite GSI) index; the write hits the base
//     table.
// No WHERE => delete all (full scan + per-item DeleteItem). A
// ConditionalCheckFailed is a ccf counter increment, not an error (D-2c-7/-8);
// success emits ONE BulkWriteOutcome{del, read, modified, ccf}.
//
// EXPLAIN asymmetry (D-2c-11, §8.3 — reproduced, not fixed):
//   * SCAN path: emit the Scan block ONCE, then RUN the scan and emit one
//     DeleteItem block per returned item across all pages (delete.cpp do_scan
//     prints Scan before the loop and the loop runs even when explaining).
//   * QUERY path: emit the Query block ONLY and do NOT run it — delete.cpp
//     do_query guards the whole execution loop behind `else (!explaining())`,
//     so EXPLAIN DELETE via query emits no DeleteItem blocks.
//   * single-write path: one DeleteItem block.
//
// Alias-prefix ordering (D-2c-10): the write template consumes the first
// prefix, the enumeration read the next.
//
// D12: never aborts; a key-schema or (non-ccf) service failure is an Error.

#ifndef DDBSH_SRC_EXECUTE_DELETE_EXECUTOR_HPP
#define DDBSH_SRC_EXECUTE_DELETE_EXECUTOR_HPP

#include <functional>
#include <string>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/outcome_sink.hpp"
#include "ddbsh/result.hpp"
#include "expressions/symbol_table.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Build the DeleteItem request TEMPLATE (port of
/// CDeleteCommand::make_delete_request): the ConditionExpression (the residual
/// WHERE predicate, if any) and the aliased ExpressionAttribute maps. The
/// caller sets `.key` per item. `st` is the write SymbolTable (first prefix);
/// `pk`/`rk` are the base-table key names. Shared with the transaction Delete
/// builder (2d) — the original's txwrite() builds the same way via this.
DeleteItemIn make_delete_request(const DeleteCmd& cmd, const std::string& pk, const std::string& rk,
                                 SymbolTable& st);

/// Execute a DELETE (direct or bulk-enumeration), emitting its outcome to
/// `sink`. `next_prefix` mints per-statement alias prefixes (write template
/// first, then enumeration read — D-2c-10). When `explain` is true the
/// serialized request block(s) are emitted as one ExplainOutcome (with the
/// query-vs-scan asymmetry above) and no mutation occurs. Returns success, or
/// the Error that aborted the command.
Result<void> execute_delete(const DeleteCmd& cmd, IDDBClient& client,
                            const std::function<std::string()>& next_prefix, bool explain,
                            OutcomeSink& sink);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_DELETE_EXECUTOR_HPP

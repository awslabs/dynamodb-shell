// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// update_executor.hpp — execute UPDATE / UPSERT against the IDDBClient.
//
// PRIVATE engine header (not installed). SDK-free port of CUpdateCommand
// (command/update.cpp, arch §7/§8). One node backs UPDATE and UPSERT (the
// `upsert` flag): UPDATE prepends `attribute_exists(pk)` to the write
// condition, UPSERT does not (and can create). The dispatch mirrors
// CUpdateCommand::run() exactly:
//   * full base-table key in the WHERE (can_update_item)  -> ONE UpdateItem
//     with the condition (read=0);
//   * else -> Where::plan_query(key schema) chooses the enumeration access
//     pattern: a QueryKeyPlan (full-PK equality + gap-free key-legal sort-key
//     prefix) -> Query, otherwise -> Scan. A `table.index` target routes the
//     read through the (possibly composite GSI) index; the write hits the base
//     table.
// Each enumerated row's base-table key drives one per-item UpdateItem; the
// residual WHERE predicate re-asserts on every write (optimistic concurrency).
// A ConditionalCheckFailed is DATA (a ccf counter increment), not an error
// (D-2c-7/-8); any other service error aborts the command without emitting the
// accounting line. Success emits ONE BulkWriteOutcome{verb, read, modified,
// ccf} (arch §7 `(n read, m modified, k ccf)`).
//
// Alias-prefix ordering (D-2c-10, byte-fidelity pinned by iut-3 / iut-25): the
// write-request SymbolTable consumes the FIRST prefix, the enumeration read's
// SymbolTable the NEXT — so `next_prefix` is called for the write template
// before the read request.
//
// EXPLAIN (D-2c-11, §8.2): the single-write path emits one UpdateItem block;
// the bulk path serializes the Query/Scan block, RUNS the read, and serializes
// one UpdateItem block per returned item (both query and scan paths run — the
// update/delete asymmetry is in DELETE, not UPDATE).
//
// D12: never aborts; a key-schema or (non-ccf) service failure is an Error.

#ifndef DDBSH_SRC_EXECUTE_UPDATE_EXECUTOR_HPP
#define DDBSH_SRC_EXECUTE_UPDATE_EXECUTOR_HPP

#include <functional>
#include <string>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/outcome_sink.hpp"
#include "ddbsh/result.hpp"
#include "expressions/symbol_table.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Build the UpdateItem request TEMPLATE (port of
/// CUpdateCommand::make_update_request): the UpdateExpression ("SET ..." |
/// "REMOVE ..."), the ConditionExpression (attribute_exists(pk) unless UPSERT,
/// AND the residual WHERE predicate), and the aliased ExpressionAttribute maps.
/// The caller sets `.key` per item. `st` is the write SymbolTable (minted from
/// the first prefix); `pk`/`rk` are the BASE-table key names. Shared with the
/// transaction Update builder (2d) — the original's txwrite() builds the same
/// way via this function.
UpdateItemIn make_update_request(const UpdateCmd& cmd, const std::string& pk, const std::string& rk,
                                 SymbolTable& st);

/// Execute an UPDATE / UPSERT (both the direct and the bulk-enumeration paths),
/// emitting its outcome to `sink`. `next_prefix` mints per-statement alias
/// prefixes (Session::next_alias_prefix); it is called for the write template
/// FIRST, then the enumeration read (D-2c-10). When `explain` is true the
/// serialized request block(s) are emitted as one ExplainOutcome and no
/// mutation occurs (the bulk path still runs the read to resolve per-item
/// keys). Returns success, or the Error that aborted the command.
Result<void> execute_update(const UpdateCmd& cmd, IDDBClient& client,
                            const std::function<std::string()>& next_prefix, bool explain,
                            OutcomeSink& sink);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_UPDATE_EXECUTOR_HPP

// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// transact_build.hpp — build TransactGetItems / TransactWriteItems sub-items.
//
// PRIVATE engine header (not installed). SDK-free ports of the original's
// txget() / txwrite() methods (select_helper.cpp, insert/update/delete.cpp),
// factored as free builders so the single-item executors and the transaction
// executor share ONE copy of the request assembly (no duplicated expression
// building — design-smell gate). Each builder:
//   * resolves the base-table key schema via the injected client (arch §10);
//   * mints exactly one alias prefix per SymbolTable it needs, in the order the
//     original did (one per inner item, parse order — D-2d-11); and
//   * returns the batch sub-item, OR an Error.
//
// The "abort" sentinel (D-2d-4/-6): a SELECT that cannot reduce to a GetItem
// (read) or an UPDATE/DELETE without a full key (write) is NOT a hard failure —
// it aborts the transaction pre-flight. These builders signal that by returning
// `fail(Errc::not_reducible_to_getitem, <message>)`, where `<message>` is the
// byte-exact diagnostic (non-empty only for the read-SELECT case; the original
// logs no message for a non-full-key UPDATE/DELETE). The executor turns that
// into an ABORT (+ optional stderr diagnostic) and keeps building the remaining
// items (collecting further diagnostics), exactly as the original did. Any
// OTHER Error (e.g. the table does not exist) is a hard failure the executor
// propagates.

#ifndef DDBSH_SRC_EXECUTE_TRANSACT_BUILD_HPP
#define DDBSH_SRC_EXECUTE_TRANSACT_BUILD_HPP

#include <functional>
#include <vector>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// The byte-exact diagnostic the original logged when a transaction SELECT
/// cannot be reduced to a GetItem (select_helper.cpp txget). Captured DATA
/// (D-2d-6): the CLI writes it to stderr; itxt-2 pins it.
inline constexpr const char* kSelectNotReducibleMessage =
    "The SELECT cannot be used in a transaction (it cannot be reduced to a GetItem() request).";

/// The byte-exact diagnostic for a bare SELECT (no EXISTS/NOT EXISTS) in a
/// write transaction (select_helper.cpp txwrite). Unreachable via the grammar
/// (a bare SELECT in a write body is a syntax error) — reproduced for
/// completeness; no path/golden reaches it (D-2d-6).
inline constexpr const char* kSelectNoExistsMessage =
    "A SELECT command without an EXISTS or NOT EXISTS cannot be used in a write transaction.";

/// Build a read-transaction Get from an inner SELECT (port of
/// CSelectHelper::txget). The SELECT must reduce to a base-table GetItem (no
/// index, full key, nothing else); otherwise fails with
/// not_reducible_to_getitem + kSelectNotReducibleMessage.
Result<TransactGet> build_transact_get(const SelectCmd& cmd, IDDBClient& client,
                                       const std::function<std::string()>& next_prefix);

/// Build the write-transaction Put sub-items for an INSERT/REPLACE (port of
/// CInsertCommand::txwrite): one TransactPut per row. INSERT keeps its
/// attribute_not_exists(pk) condition (one alias prefix per INSERT row);
/// REPLACE is unconditional (no prefix).
Result<std::vector<TransactPut>> build_transact_puts(
    const InsertCmd& cmd, IDDBClient& client, const std::function<std::string()>& next_prefix);

/// Build a write-transaction Update from an UPDATE/UPSERT (port of
/// CUpdateCommand::txwrite). Requires a full base-table key (can_update_item);
/// otherwise fails with not_reducible_to_getitem and an EMPTY message (the
/// original aborts silently here).
Result<TransactUpdate> build_transact_update(const UpdateCmd& cmd, IDDBClient& client,
                                             const std::function<std::string()>& next_prefix);

/// Build a write-transaction Delete from a DELETE (port of
/// CDeleteCommand::txwrite). Requires a full base-table key (can_delete_item);
/// otherwise fails with not_reducible_to_getitem and an EMPTY message.
Result<TransactDelete> build_transact_delete(const DeleteCmd& cmd, IDDBClient& client,
                                             const std::function<std::string()>& next_prefix);

/// Build a write-transaction ConditionCheck from an EXISTS/NOT EXISTS SELECT
/// (port of CSelectHelper::txwrite). The check function is attribute_exists
/// (EXISTS) / attribute_not_exists (NOT EXISTS); the condition is the residual
/// WHERE filter ` AND `-joined with check_func(#pk) (projection `*`) or one
/// check_func(#p) per projected attribute (concatenated WITHOUT a separator —
/// the original's quirk, reproduced). A SELECT with exists_mode == none fails
/// with kSelectNoExistsMessage (unreachable via the grammar).
Result<TransactConditionCheck> build_transact_condition_check(
    const SelectCmd& cmd, IDDBClient& client, const std::function<std::string()>& next_prefix);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_TRANSACT_BUILD_HPP

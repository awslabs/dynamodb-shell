// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// transact_executor.hpp — execute a transaction against the IDDBClient.
//
// PRIVATE engine header (not installed). SDK-free port of CTransactReadCommand
// / CTransactWriteCommand (transact-read.hpp / transact-write.hpp, arch §7).
// One entry point handles both the read (TransactGetItems) and write
// (TransactWriteItems) families; the TransactCmd's `access` discriminant
// (fixed by the grammar) selects the path.
//
// Behavior (D-2d-4 .. D-2d-12):
//   * Empty `begin; rollback;` -> EmptyOutcome (no output, itxt-1).
//   * Pre-flight build of every sub-item (both families): a non-reducible
//     inner SELECT (read) or a non-full-key UPDATE/DELETE (write) aborts the
//     transaction pre-flight with no API call; the read-SELECT case also
//     records a byte-exact stderr diagnostic (D-2d-6). A ROLLBACK terminator
//     aborts likewise.
//   * ABORT has two paths, both printing `ABORT` to stdout: the pre-flight
//     abort is success DATA (TransactOutcome, exit 0); a service cancellation
//     emits ABORT via the sink THEN returns the composed "Transaction failed."
//     Error (exit nonzero) — D-2d-5.
//   * Read success -> items only via SelectOutcome, NO status token; write
//     success -> StatusOutcome{"COMMIT"} (D-2d-8).
//   * EXPLAIN -> one serialized batch block (TransactGetItems /
//     TransactWriteItems), no enumeration; an aborted/rolled-back EXPLAIN
//     prints `ABORT` (D-2d-12).
//
// D12: never aborts the process; a hard failure (e.g. a missing table) or a
// service cancellation is returned as an Error.

#ifndef DDBSH_SRC_EXECUTE_TRANSACT_EXECUTOR_HPP
#define DDBSH_SRC_EXECUTE_TRANSACT_EXECUTOR_HPP

#include <functional>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/outcome_sink.hpp"
#include "ddbsh/result.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Execute a transaction, emitting its outcome(s) to `sink`. `next_prefix`
/// mints per-statement alias prefixes (Session::next_alias_prefix), consumed
/// per inner sub-item in parse order (D-2d-11). When `explain` is true the one
/// serialized batch request is emitted as an ExplainOutcome and no API call is
/// made (a rolled-back / pre-flight-aborted EXPLAIN emits ABORT). Returns
/// success, or the Error that aborted the command (service cancellation).
Result<void> execute_transact(const TransactCmd& cmd, IDDBClient& client,
                              const std::function<std::string()>& next_prefix, bool explain,
                              OutcomeSink& sink);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_TRANSACT_EXECUTOR_HPP

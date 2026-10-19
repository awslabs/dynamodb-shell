// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// show_commands_executor.hpp — execute SHOW BACKUPS / SHOW LIMITS /
// SHOW CREATE TABLE against the IDDBClient (2f).
//
// PRIVATE engine header (not installed). SDK-free ports of the original
// show_backups / show_limits / show_create_table commands (arch §7).
//
// FIDELITY NOTE (supersedes the design's R2/D-2f-6 and the D-2f-8 SHOW-under-
// EXPLAIN provisions): in the oracle grammar, `explain_command` wraps only
// ddl_command / dml_command / transact_command (parser.y:463). SHOW commands
// live under show_command, which EXPLAIN does NOT wrap — so `EXPLAIN SHOW ...`
// is a syntax error in the oracle, and the `explaining()` branches in the
// oracle's show_*.cpp are unreachable dead code. The reimplementation matches:
// SHOW commands are not explainable (grammar), so these executors have no
// EXPLAIN path. (The backup/restore VERBS — BACKUP / DROP BACKUP /
// DESCRIBE BACKUP / RESTORE — ARE under ddl_command and so ARE explainable;
// their EXPLAIN lives in backup_restore_executor.)
//
//   * SHOW BACKUPS: paginated ListBackups; one ReportOutcome line per summary.
//   * SHOW LIMITS: DescribeLimits + paged ListTables + describe_table per table
//     to sum provisioned/GSI throughput. The `Region:` line comes from the
//     session region, threaded as a parameter (D-2f-9).
//   * SHOW CREATE TABLE: DescribeTable (+ DescribeTimeToLive probe) then the
//     reconstructed CREATE TABLE DDL. Guards: missing table / non-ACTIVE table
//     return an Error.
//
// D12: never aborts; service failures are returned as Errors.

#ifndef DDBSH_SRC_EXECUTE_SHOW_COMMANDS_EXECUTOR_HPP
#define DDBSH_SRC_EXECUTE_SHOW_COMMANDS_EXECUTOR_HPP

#include <string>

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// SHOW [type] BACKUPS [<t>] -> paginated ListBackups. ReportOutcome of rows.
Result<ExecOutcome> execute_show_backups(const ShowBackupsCmd& cmd, IDDBClient& client);

/// SHOW LIMITS -> DescribeLimits + paged ListTables/DescribeTable. `region` is
/// the session's current region, used for the `Region:` line (D-2f-9).
Result<ExecOutcome> execute_show_limits(const ShowLimitsCmd& cmd, IDDBClient& client,
                                        const std::string& region);

/// SHOW CREATE TABLE [IF NOT EXISTS][NOWAIT] <t> -> DescribeTable (+ TTL probe)
/// then the reconstructed DDL (one ReportOutcome line).
Result<ExecOutcome> execute_show_create_table(const ShowCreateTableCmd& cmd, IDDBClient& client);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_SHOW_COMMANDS_EXECUTOR_HPP

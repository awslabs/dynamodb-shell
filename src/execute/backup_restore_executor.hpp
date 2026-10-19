// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// backup_restore_executor.hpp — execute the BACKUP / DROP BACKUP /
// DESCRIBE BACKUP / RESTORE family against the IDDBClient (2f).
//
// PRIVATE engine header (not installed). SDK-free ports of the original
// create_backup / drop_backup / describe_backup / restore_backup / restore_pitr
// commands (arch §7). The four status-token verbs (BACKUP / DROP BACKUP /
// RESTORE both forms) emit a StatusOutcome on success; DESCRIBE BACKUP emits a
// multi-line ReportOutcome (§5). Under EXPLAIN each serializes exactly one
// request block (RESTORE-to-PITR carries the quirky Request-suffixed label,
// R1/D-2f-5); DROP BACKUP's block is DeleteBackup (correcting the stale arch
// §12 #1 claim — D-2f-7).
//
// D12: never aborts; a service failure is returned as an Error (the CLI adapter
// composes the user-facing message; 2f error text is not golden-pinned, D-2f-3).

#ifndef DDBSH_SRC_EXECUTE_BACKUP_RESTORE_EXECUTOR_HPP
#define DDBSH_SRC_EXECUTE_BACKUP_RESTORE_EXECUTOR_HPP

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/result.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// BACKUP TABLE <t> CALLED <name> -> CreateBackup. StatusOutcome{"BACKUP"} on
/// success; under EXPLAIN a single `CreateBackup(...)` block (no status).
Result<ExecOutcome> execute_create_backup(const CreateBackupCmd& cmd, IDDBClient& client,
                                          bool explain);

/// DROP BACKUP <arn> -> DeleteBackup. StatusOutcome{"DROP BACKUP"} on success;
/// under EXPLAIN a single `DeleteBackup(...)` block (D-2f-7).
Result<ExecOutcome> execute_drop_backup(const DropBackupCmd& cmd, IDDBClient& client, bool explain);

/// DESCRIBE BACKUP <arn> -> DescribeBackup. ReportOutcome of the backup detail
/// lines (§5) on success; under EXPLAIN a single `DescribeBackup(...)` block.
Result<ExecOutcome> execute_describe_backup(const DescribeBackupCmd& cmd, IDDBClient& client,
                                            bool explain);

/// RESTORE (both forms). from_backup -> RestoreTableFromBackup (block label
/// `RestoreTableFromBackup`); to_pitr -> RestoreTableToPointInTime (block label
/// `RestoreTableToPointInTimeRequest`, R1/D-2f-5). StatusOutcome{"RESTORE"} on
/// success; under EXPLAIN a single request block (no status).
Result<ExecOutcome> execute_restore(const RestoreCmd& cmd, IDDBClient& client, bool explain);

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_BACKUP_RESTORE_EXECUTOR_HPP

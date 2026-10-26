// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// backup_restore_executor.cpp — see backup_restore_executor.hpp.

#include "execute/backup_restore_executor.hpp"

#include <string>
#include <utility>

namespace ddbsh::engine {

namespace {

/// A single serialized request EXPLAIN block (all 2f direct requests use the
/// pretty format — the original wrapped them with explain_string, not
/// strip_newlines).
Result<ExecOutcome> explain_one(IDDBClient& client, const char* api_name,
                                const ExplainRequest& request) {
    auto body = client.explain_request(request);
    if (!body.has_value()) {
        return fail(body.error());
    }
    ExplainOutcome outcome;
    outcome.blocks.push_back(
        ExplainBlock{api_name, std::move(*body), ExplainBlock::Format::pretty});
    return ExecOutcome{std::move(outcome)};
}

}  // namespace

Result<ExecOutcome> execute_create_backup(const CreateBackupCmd& cmd, IDDBClient& client,
                                          bool explain) {
    CreateBackupIn request;
    request.table_name = cmd.table;
    request.backup_name = cmd.backup_name;
    if (explain) {
        return explain_one(client, "CreateBackup", request);
    }
    auto done = client.create_backup(request);
    if (!done.has_value()) {
        return fail(done.error());
    }
    return ExecOutcome{StatusOutcome{"BACKUP"}};
}

Result<ExecOutcome> execute_drop_backup(const DropBackupCmd& cmd, IDDBClient& client,
                                        bool explain) {
    DeleteBackupIn request;
    request.backup_arn = cmd.backup_arn;
    if (explain) {
        // EXPLAIN emits `DeleteBackup(...)` — it IS a DeleteBackup call. The
        // arch §12 #1 claim that it prints `DescribeBackup(...)` is stale vs the
        // oracle (D-2f-7).
        return explain_one(client, "DeleteBackup", request);
    }
    auto done = client.delete_backup(request);
    if (!done.has_value()) {
        return fail(done.error());
    }
    return ExecOutcome{StatusOutcome{"DROP BACKUP"}};
}

Result<ExecOutcome> execute_describe_backup(const DescribeBackupCmd& cmd, IDDBClient& client,
                                            bool explain) {
    DescribeBackupIn request;
    request.backup_arn = cmd.backup_arn;
    if (explain) {
        return explain_one(client, "DescribeBackup", request);
    }
    auto described = client.describe_backup(request);
    if (!described.has_value()) {
        return fail(described.error());
    }
    const DescribeBackupOut& b = described.value();

    ReportOutcome report;
    auto& lines = report.lines;
    lines.push_back("Table Name: " + b.source_table_name);
    lines.push_back("Table Id: " + b.source_table_id);  // correct label (D-2f-7)
    lines.push_back("Table Arn: " + b.source_table_arn);
    lines.push_back("Table Size (bytes): " + std::to_string(b.table_size_bytes));
    lines.push_back("Item Count: " + std::to_string(b.item_count));
    {
        // Key line: KeyType name + " " + attribute, joined by ", ", wrapped as
        // `Table Key: ( <...> )` (the original describe_backup.cpp loop).
        std::string key;
        for (const auto& kse : b.key_schema) {
            if (!key.empty()) {
                key += ", ";
            }
            key += kse.key_type + " " + kse.attribute_name;
        }
        lines.push_back("Table Key: ( " + key + " )");
    }
    lines.push_back("Table Creation Date/Time: " + b.table_creation_datetime);
    if (b.billing_mode.has_value()) {
        lines.push_back("Billing Mode: " + *b.billing_mode);
    }
    if (b.provisioned.has_value()) {
        lines.push_back("Provisioned Throughput: " + std::to_string(b.provisioned->first) +
                        " RCU, " + std::to_string(b.provisioned->second) + " WCU");
    }
    lines.push_back("Backup Name: " + b.backup_name);
    lines.push_back("Backup Arn: " + b.backup_arn);
    lines.push_back("Backup Size (bytes): " + std::to_string(b.backup_size_bytes));
    lines.push_back("Backup Status: " + b.backup_status);
    lines.push_back("Backup Type: " + b.backup_type);
    lines.push_back("Backup Creation Date/Time: " + b.backup_creation_datetime);
    lines.push_back("Backup Expiry Date/Time: " + b.backup_expiry_datetime);
    return ExecOutcome{std::move(report)};
}

Result<ExecOutcome> execute_restore(const RestoreCmd& cmd, IDDBClient& client, bool explain) {
    if (cmd.kind == RestoreCmd::Kind::from_backup) {
        RestoreFromBackupIn request;
        request.target_table_name = cmd.target;  // N1: target after K_TABLE
        request.backup_arn = cmd.source;         // N1: source after K_FROM
        if (explain) {
            return explain_one(client, "RestoreTableFromBackup", request);
        }
        auto done = client.restore_table_from_backup(request);
        if (!done.has_value()) {
            return fail(done.error());
        }
        return ExecOutcome{StatusOutcome{"RESTORE"}};
    }

    RestoreToPitrIn request;
    request.target_table_name = cmd.target;  // N1: target after K_TABLE
    request.source_table_name = cmd.source;  // N1: source after K_FROM
    request.restore_datetime = cmd.timestamp;
    if (explain) {
        // R1/D-2f-5: the RESTORE-to-PITR EXPLAIN label is Request-suffixed
        // (`RestoreTableToPointInTimeRequest`), the sole restore quirk.
        return explain_one(client, "RestoreTableToPointInTimeRequest", request);
    }
    auto done = client.restore_table_to_point_in_time(request);
    if (!done.has_value()) {
        return fail(done.error());
    }
    return ExecOutcome{StatusOutcome{"RESTORE"}};
}

}  // namespace ddbsh::engine

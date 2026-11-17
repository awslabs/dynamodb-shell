// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// wire_client.hpp — an IDDBClient that drives the SDK-free ddbsh-wire codec
// over a Transport (P6b; D-P6b-4).
//
// WireClient is the SDK-free counterpart to ddbsh::aws::AwsDDBClient: where the
// AWS client translates each op through the AWS SDK, WireClient serializes it
// to AWS-JSON-1.0 (wire::serialize, P6a), sends it through a Transport
// (transport.hpp), and parses the response (wire::parse_*, P6b). The browser
// (Emscripten) engine target constructs a WireClient over an in-WASM/fetch
// Transport and drives a ddbsh::Session with it — no AWS SDK, no terminal.
//
// SCOPE (P6c-b). The full IDDBClient surface is implemented SDK-free: the six
// data-plane ops (P6b), the control plane (create/drop/alter table, the
// describe family, show-limits/tables, backup/restore), transactions, vector
// search, the two table waiters, and explain_request for every ExplainRequest
// alternative. ListTables/ListBackups page internally (the AWS adapter's paging
// loops reproduced over the transport); five ops (the two transactions,
// UpdateTable, UpdateTimeToLive, UpdateContinuousBackups) re-wrap a mapped Error
// in the adapter's composed failure message (§4.3). The browser (Emscripten)
// engine target constructs a WireClient over an in-WASM/fetch Transport and
// drives a ddbsh::Session with it — no AWS SDK, no terminal.
//
// Ownership: WireClient borrows the Transport&, exactly as Session borrows
// IDDBClient& — the embedder guarantees the transport outlives the client. No
// owning raw pointer crosses the boundary.

#ifndef DDBSH_WIRE_WIRE_CLIENT_HPP
#define DDBSH_WIRE_WIRE_CLIENT_HPP

#include <chrono>
#include <string>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/wire/transport.hpp"

namespace ddbsh::wire {

/// An IDDBClient backed by the SDK-free ddbsh-wire codec and a Transport.
class WireClient : public ddbsh::IDDBClient {
public:
    /// @param transport  the wire backend; must outlive this client (borrowed).
    explicit WireClient(Transport& transport) : transport_(transport) {}

    // ---- connect: record state only (no API call) --------------------------
    // The WASM build resolves the endpoint to a transport selection (D-P6-A8);
    // in P6b (host, single injected Transport) this only records region/endpoint
    // so a CLI/embedder prompt hint can read it. Always succeeds (the original's
    // reconnect was non-fatal).
    Result<void> connect(const std::string& region, const std::string& endpoint) override;

    // ---- data-plane ops (fully implemented: serialize -> send -> parse) -----
    Result<GetItemOut> get_item(const GetItemIn& in) override;
    Result<QueryOut> query(const QueryIn& in) override;
    Result<ScanOut> scan(const ScanIn& in) override;
    Result<PutItemOut> put_item(const PutItemIn& in) override;
    Result<UpdateItemOut> update_item(const UpdateItemIn& in) override;
    Result<DeleteItemOut> delete_item(const DeleteItemIn& in) override;

    // ---- SDK-free EXPLAIN for the six data-plane alternatives ---------------
    // Returns serialize(x).body for a data-plane alternative (byte-proven == the
    // SDK's SerializePayload by P6a's differential gate); the other alternatives
    // return not_implemented until P6c extends the visit.
    Result<std::string> explain_request(const ExplainRequest& req) override;

    // ---- control plane / backup-restore / transactions / waiters (P6c-b) ---
    Result<CreateTableOut> create_table(const CreateTableIn&) override;
    Result<UpdateTableOut> update_table(const UpdateTableIn&) override;
    Result<UpdateTimeToLiveOut> update_time_to_live(const UpdateTimeToLiveIn&) override;
    Result<UpdateContinuousBackupsOut> update_continuous_backups(
        const UpdateContinuousBackupsIn&) override;
    Result<CreateBackupOut> create_backup(const CreateBackupIn&) override;
    Result<DeleteBackupOut> delete_backup(const DeleteBackupIn&) override;
    Result<DescribeBackupOut> describe_backup(const DescribeBackupIn&) override;
    Result<RestoreFromBackupOut> restore_table_from_backup(const RestoreFromBackupIn&) override;
    Result<RestoreToPitrOut> restore_table_to_point_in_time(const RestoreToPitrIn&) override;
    Result<DescribeLimitsOut> describe_limits(const DescribeLimitsIn&) override;
    Result<ListBackupsOut> list_backups(const ListBackupsIn&) override;
    Result<DeleteTableOut> delete_table(const DeleteTableIn&) override;
    Result<ListTablesOut> list_tables(const ListTablesIn&) override;
    Result<DescribeTableOut> describe_table(const DescribeTableIn&) override;
    Result<SearchVectorsOut> search_vectors(const SearchVectorsIn&) override;
    Result<TransactGetItemsOut> transact_get_items(const TransactGetItemsIn&) override;
    Result<TransactWriteItemsOut> transact_write_items(const TransactWriteItemsIn&) override;
    Result<TimeToLiveOut> describe_time_to_live(const DescribeTimeToLiveIn&) override;
    Result<ContinuousBackupsOut> describe_continuous_backups(
        const DescribeContinuousBackupsIn&) override;
    Result<TableReplicaAutoScalingOut> describe_table_replica_auto_scaling(
        const DescribeTableReplicaAutoScalingIn&) override;
    Result<void> wait_for_table_active(const std::string& table_name) override;
    Result<void> wait_for_table_gone(const std::string& table_name) override;

protected:
    /// The waiter poll delay (D-P6c-6). Default: emscripten_sleep under
    /// Asyncify (so the browser event loop keeps turning), std::this_thread::
    /// sleep_for on the host. The single platform `#if` lives in this one
    /// method; nothing else in WireClient is platform-coupled. A test overrides
    /// it to a no-op so the waiter loops run instantly (no real sleep in CI).
    virtual void poll_delay(std::chrono::milliseconds interval);

private:
    Transport& transport_;
    std::string region_;
    std::string endpoint_;
};

}  // namespace ddbsh::wire

#endif  // DDBSH_WIRE_WIRE_CLIENT_HPP

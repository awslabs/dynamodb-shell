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
// (Emscripten) engine target constructs a WireClient over the JS SDK bridge
// Transport (web/js_sdk_transport.hpp) and drives a ddbsh::Session with it — no
// AWS SDK for C++, no terminal.
//
// SCOPE (P6c-b). The full IDDBClient surface is implemented SDK-free: the six
// data-plane ops (P6b), the control plane (create/drop/alter table, the
// describe family, show-limits/tables, backup/restore), transactions, vector
// search, the two table waiters, and explain_request for every ExplainRequest
// alternative. ListTables/ListBackups page internally (the AWS adapter's paging
// loops reproduced over the transport); five ops (the two transactions,
// UpdateTable, UpdateTimeToLive, UpdateContinuousBackups) re-wrap a mapped Error
// in the adapter's composed failure message (§4.3).
//
// Ownership: WireClient borrows the Transport&, exactly as Session borrows
// IDDBClient& — the embedder guarantees the transport outlives the client. No
// owning raw pointer crosses the boundary.

#ifndef DDBSH_WIRE_WIRE_CLIENT_HPP
#define DDBSH_WIRE_WIRE_CLIENT_HPP

#include <chrono>
#include <random>
#include <string>
#include <string_view>

#include "ddbsh/iddbclient.hpp"
#include "ddbsh/wire/transport.hpp"

namespace ddbsh::wire {

/// A serialized DynamoDB request (operation + body). Defined in codec.hpp;
/// forward-declared here so the private round_trip helper can take it by
/// reference without the header depending on the full codec surface.
struct WireRequest;

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

    // ---- SDK-free EXPLAIN for every ExplainRequest alternative (P6c-a) ------
    // Returns serialize(x).body for every alternative — byte-proven == the SDK's
    // SerializePayload by the differential gate (P6c-a extended the P6a corpus to
    // all serializable ops). No alternative returns not_implemented. The one
    // fallible arm, RestoreToPitrIn, validates the restore timestamp via
    // iso8601_to_epoch first and returns the adapter's exact
    // "Invalid restore timestamp: …" invalid_argument Error before serializing.
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

    // ---- SHOW CONNECTION descriptor (P6f, D-P6f-9) -------------------------
    // Reports the wire composition: the Transport's descriptor (its kind +
    // backend binding). The engine's SHOW CONNECTION arm prefixes the library
    // version and the session region/endpoint; this contributes the transport
    // half.
    std::string connection_descriptor() const override;

protected:
    /// The backoff sleep, shared by the two table waiters and the retry path
    /// (D-P6c-6; extended for HT-1 retry). Default: emscripten_sleep under
    /// Asyncify (so the browser event loop keeps turning and the transport's
    /// fetch makes progress), std::this_thread::sleep_for on the host. The single
    /// platform `#if` in WireClient lives in this one method; nothing else is
    /// platform-coupled. A test overrides it to a no-op so the waiter and retry
    /// loops run instantly (no real sleep in CI).
    virtual void backoff_sleep(std::chrono::milliseconds interval);

    /// A uniform random fraction in [0, 1) for full-jitter backoff (HT-1 §4.7).
    /// Default: a per-instance mt19937_64 seeded once from random_device. A test
    /// overrides it (e.g. to a constant) so retry timing — and thus attempt
    /// counts — is deterministic. Per-instance (no global state): the engine may
    /// run multiple sessions concurrently (shared-steering tenet).
    virtual double jitter_unit();

    /// Retry attempt cap: 1 initial attempt + (cap - 1) retries. Default 11 is
    /// parity with the AWS SDK DefaultRetryStrategy (maxRetries = 10) the
    /// SDK-backed CLI used below the IDDBClient seam, so the SDK-free wire path
    /// gets the same resilience ceiling (HT-1 §4.5). A test subclass lowers it to
    /// exercise exhaustion quickly.
    unsigned max_retry_attempts_ = 11;

private:
    /// Send one request through the transport, retrying a retryable service
    /// error (a `__type` document whose exception name is transient, per
    /// is_transient_wire_error) with bounded exponential backoff + full jitter
    /// (HT-1 §4.1). Returns the final response body: a success body, a terminal
    /// error document, or the last retryable error document after the attempt
    /// cap (so parse_* maps the exact same error string the one-shot path
    /// would). An opaque transport-level failure (no `__type` to classify) is
    /// returned immediately (D-HT1-3). This is the single retry seam every
    /// non-waiter op routes through; the two waiters keep their own poll loop.
    Result<std::string> send_retrying(std::string_view operation, std::string_view body);

    /// Sleep the full-jitter backoff interval for the given 0-based attempt
    /// (HT-1 §4.5): a uniform draw in [0, min(cap, base * 2^attempt)], base
    /// 50ms, cap 1000ms. Calls backoff_sleep for the actual wait.
    void retry_backoff(unsigned attempt);

    /// serialize -> send_retrying -> parse for an op whose response maps
    /// straight to its *Out (no composed error, no paging). Defined in
    /// wire_client.cpp; every instantiation is internal to that TU.
    template <class Out>
    Result<Out> round_trip(const WireRequest& req, Result<Out> (*parse)(const std::string&));

    Transport& transport_;
    std::string region_;
    std::string endpoint_;
    std::mt19937_64 rng_{std::random_device{}()};
};

}  // namespace ddbsh::wire

#endif  // DDBSH_WIRE_WIRE_CLIENT_HPP

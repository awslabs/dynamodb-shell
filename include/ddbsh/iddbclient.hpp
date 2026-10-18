// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// iddbclient.hpp — the injected DynamoDB client seam (T7, D11).
//
// A narrow, pure interface the engine drives. Request/response types are the
// library's OWN SDK-free POD structs (built from ddbsh::Value), so public
// headers never mention Aws::. The CLI-side AwsDDBClient translates to/from
// the AWS SDK; the test FakeDDBClient fills these structs directly, so the
// whole engine is unit-testable with no AWS account.
//
// 2a scope: only the handful of ops the walking skeleton and near-term ports
// need are declared. update/delete/transact/describe/create/drop/backup/pitr
// and a CancelToken land per sub-phase (2b-2d).

#ifndef DDBSH_IDDBCLIENT_HPP
#define DDBSH_IDDBCLIENT_HPP

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ddbsh/result.hpp"
#include "ddbsh/table_schema.hpp"
#include "ddbsh/value.hpp"

namespace ddbsh {

/// A single returned row: attribute name -> Value (source order; Renderer
/// sorts).
struct ItemRow {
    ValueMap attributes;
};

/// ExpressionAttributeNames: token (`#a...`) -> real attribute name, in token
/// order (the engine builds these from the SymbolTable, which is ordered).
using ExpressionAttributeNames = std::vector<std::pair<std::string, std::string>>;

/// How much consumed-capacity detail a read/write asks the service to return.
/// Mirrors the SDK's ReturnConsumedCapacity; the Renderer maps it to the exact
/// "NONE"/"TOTAL"/"INDEXES" token in EXPLAIN output (arch §8). Default NONE; a
/// ratelimit forces TOTAL at plan time.
enum class ReturnConsumedCapacity { none, total, indexes };

/// Consumed-capacity figures a read/write returns when ReturnConsumedCapacity
/// is not NONE. Each field is optional because the SDK sets them independently
/// (a TableName plus any of CapacityUnits / ReadCapacityUnits /
/// WriteCapacityUnits). The engine totalizes these across paginated Query/Scan
/// responses (arch §10, CConsumedCapacityTotalizer) and the Renderer formats
/// the `SELECT (table, cu, rcu, wcu)` line (arch §9) only when the user asked
/// for it (RETURN CONSUMED CAPACITY) — a ratelimit forces the service to return
/// capacity but does not display it.
struct ConsumedCapacity {
    std::optional<std::string> table_name;
    std::optional<double> capacity_units;
    std::optional<double> read_capacity_units;
    std::optional<double> write_capacity_units;
};

// ---- GetItem ---------------------------------------------------------------
// The engine aliases projection attributes through a SymbolTable, so the
// request carries the serialized ProjectionExpression and the matching
// ExpressionAttributeNames (not a raw column list) — this keeps the aliasing
// deterministic and shared with the Query/Scan paths for byte-identical
// EXPLAIN output (arch §6/§8). get_gud_key supplies the literal Key map (no
// aliases), matching the original.
struct GetItemIn {
    std::string table_name;
    ValueMap key;
    std::optional<std::string> projection_expression;
    ExpressionAttributeNames expression_attribute_names;
    bool consistent_read = false;
    ReturnConsumedCapacity return_consumed_capacity = ReturnConsumedCapacity::none;
};
struct GetItemOut {
    std::optional<ItemRow> item;
    std::optional<ConsumedCapacity> consumed_capacity;
};

// ---- Query -----------------------------------------------------------------
struct QueryIn {
    std::string table_name;
    std::optional<std::string> index_name;
    std::string key_condition_expression;
    std::optional<std::string> filter_expression;
    std::optional<std::string> projection_expression;
    ExpressionAttributeNames expression_attribute_names;
    ValueMap expression_attribute_values;
    bool consistent_read = false;
    ReturnConsumedCapacity return_consumed_capacity = ReturnConsumedCapacity::none;
    std::optional<ValueMap> exclusive_start_key;  // pagination cursor
};
struct QueryOut {
    std::vector<ItemRow> items;
    std::optional<ValueMap> last_evaluated_key;
    std::optional<ConsumedCapacity> consumed_capacity;
};

// ---- Scan ------------------------------------------------------------------
struct ScanIn {
    std::string table_name;
    std::optional<std::string> index_name;
    std::optional<std::string> filter_expression;
    std::optional<std::string> projection_expression;
    ExpressionAttributeNames expression_attribute_names;
    ValueMap expression_attribute_values;
    bool consistent_read = false;
    ReturnConsumedCapacity return_consumed_capacity = ReturnConsumedCapacity::none;
    std::optional<ValueMap> exclusive_start_key;
};
struct ScanOut {
    std::vector<ItemRow> items;
    std::optional<ValueMap> last_evaluated_key;
    std::optional<ConsumedCapacity> consumed_capacity;
};

// ---- PutItem ---------------------------------------------------------------
struct PutItemIn {
    std::string table_name;
    ValueMap item;
    std::optional<std::string> condition_expression;
    std::vector<std::pair<std::string, std::string>> expression_attribute_names;
    ValueMap expression_attribute_values;
};
struct PutItemOut {};

// ---- UpdateItem / DeleteItem ----------------------------------------------
// UPDATE/UPSERT and DELETE issue one UpdateItem/DeleteItem per addressed item
// (arch §7). The engine builds the UpdateExpression / ConditionExpression and
// aliases every attribute/value through a SymbolTable, so the request carries
// the serialized strings plus the matching ExpressionAttribute* maps (not raw
// column lists) — identical aliasing to the Query/Scan/Put paths for
// byte-exact EXPLAIN (arch §6/§8). `key` is the literal item key (get_gud_key
// on the single-write path, or an enumerated row's key on the bulk path).

/// RETURN VALUES selector (public seam mirror of engine::ReturnValue). Threaded
/// into UpdateItem/DeleteItem for EXPLAIN fidelity; the returned-item DISPLAY
/// is deferred to 2g (the ddbsh-rv suite owns it). Default `none`.
enum class ReturnValue { none, all_old, all_new, updated_old, updated_new };

struct UpdateItemIn {
    std::string table_name;
    ValueMap key;
    std::string update_expression;  ///< "SET ..." | "REMOVE ..."
    std::optional<std::string> condition_expression;
    ExpressionAttributeNames expression_attribute_names;
    ValueMap expression_attribute_values;
    ReturnValue return_values = ReturnValue::none;
    ReturnConsumedCapacity return_consumed_capacity = ReturnConsumedCapacity::none;
};
struct UpdateItemOut {
    std::optional<ItemRow> attributes;  ///< RETURN VALUES payload (ignored until 2g)
    std::optional<ConsumedCapacity> consumed_capacity;
};

struct DeleteItemIn {
    std::string table_name;
    ValueMap key;
    std::optional<std::string> condition_expression;
    ExpressionAttributeNames expression_attribute_names;
    ValueMap expression_attribute_values;
    ReturnValue return_values = ReturnValue::none;
    ReturnConsumedCapacity return_consumed_capacity = ReturnConsumedCapacity::none;
};
struct DeleteItemOut {
    std::optional<ItemRow> attributes;
    std::optional<ConsumedCapacity> consumed_capacity;
};

// ---- DeleteTable -----------------------------------------------------------
// DROP TABLE issues DeleteTable then (unless NOWAIT) waits for the table to be
// gone (arch §7/§10). The request carries only the table name; the SDK-free
// struct keeps the public seam AWS-type-free (D11).
struct DeleteTableIn {
    std::string table_name;
};
struct DeleteTableOut {};

// ---- CreateTable -----------------------------------------------------------
// CREATE TABLE issues CreateTable then (unless NOWAIT) waits for the table to
// become ACTIVE (arch §7/§10). The request carries the full SDK-free table
// definition (ddbsh/table_schema.hpp) — the same value types the parse result
// builds — so the public seam stays AWS-type-free (D11); the CLI-side adapter
// translates it to Aws::DynamoDB::Model::CreateTableRequest.
//
// `billing == nullopt` means "no billing clause": the service defaults to
// on-demand (PAY_PER_REQUEST), which the adapter applies — matching the
// original create_table.cpp, which set PAY_PER_REQUEST when no billing clause
// was given (arch §8). A present `billing` with mode pay_per_request and an
// empty `od` is a bare `BILLING MODE ON DEMAND`; both `od` units == -1 is
// `THROUGHPUT UNLIMITED` (arch §8).
struct CreateTableIn {
    std::string table_name;
    std::vector<AttributeDef> attribute_definitions;
    KeySchema key_schema;
    std::optional<BillingModeAndThroughput> billing;  ///< nullopt => default on-demand
    std::optional<WarmThroughput> warm;
    std::vector<Gsi> gsis;
    std::vector<Lsi> lsis;
    std::optional<StreamSpec> stream;
    std::optional<SseSpec> sse;
    TableClass table_class = TableClass::standard;
    bool deletion_protection = false;
    std::vector<Tag> tags;
};
struct CreateTableOut {};

// ---- UpdateTable -----------------------------------------------------------
// ALTER TABLE SET ... issues UpdateTable (arch §7, the original
// CUpdateTableCommand). Unlike CreateTable, every field is OPTIONAL and set
// only when the clause provided it: the original built one UpdateTableRequest
// and set exactly the attributes the clause selected, leaving the rest unset so
// the SDK omits them from the payload. The request carries SDK-free value
// types (ddbsh/table_schema.hpp) shared with the parse result, keeping the
// public seam AWS-type-free (D11); the CLI adapter translates it to
// Aws::DynamoDB::Model::UpdateTableRequest, setting only the present fields.
//
// Billing fidelity (arch §8): a present `billing` always sets BOTH BillingMode
// and the matching throughput (provisioned => ProvisionedThroughput;
// on-demand => OnDemandThroughput with the -1 clear-one / UNLIMITED clear-both
// sentinels). `table_class` / `deletion_protection` are optional<> (NOT a
// defaulted value) so "clause absent" serializes to NO TableClass /
// DeletionProtectionEnabled in the request, matching the original's unset
// fields. `attribute_definitions` + `gsi_updates` carry the CREATE / UPDATE /
// DROP GSI actions (2e-2); the replica ReplicationGroupUpdate lands in 2e-5.
struct UpdateTableIn {
    std::string table_name;
    std::optional<BillingModeAndThroughput> billing;  ///< SET billing_mode_and_throughput
    std::optional<WarmThroughput> warm;               ///< SET warm_throughput
    std::optional<SseSpec> sse;                       ///< SET sse_specification
    std::optional<StreamSpec> stream;                 ///< SET stream_specification
    std::optional<TableClass> table_class;            ///< SET table_class (nullopt => unset)
    std::optional<bool> deletion_protection;  ///< SET deletion protection (nullopt => unset)
    std::vector<AttributeDef> attribute_definitions;  ///< new key attrs for CREATE GSI
    std::vector<GsiUpdate> gsi_updates;               ///< CREATE / UPDATE / DROP GSI actions
};
struct UpdateTableOut {};

// ---- UpdateTimeToLive ------------------------------------------------------
// ALTER TABLE SET TTL ( attr ) / SET TTL DISABLED issues UpdateTimeToLive after
// a DescribeTimeToLive probe (arch §7, the original CUpdateTableTTLCommand).
// `enabled` sets TimeToLiveSpecification.Enabled; `attribute_name` is the TTL
// attribute — the user-supplied name on enable, or the existing name (read back
// from DescribeTimeToLive) on disable. The SDK-free struct keeps the seam
// AWS-type-free (D11); the CLI adapter translates it to
// Aws::DynamoDB::Model::UpdateTimeToLiveRequest and composes the byte-exact
// "Error updating TTL. ..." failure message the original logged (D-2e-13).
struct UpdateTimeToLiveIn {
    std::string table_name;
    bool enabled = false;
    std::string attribute_name;
};
struct UpdateTimeToLiveOut {};

// ---- UpdateContinuousBackups (PITR) ----------------------------------------
// ALTER TABLE SET PITR ENABLED / SET PITR DISABLED issues UpdateContinuousBackups
// (arch §7, the original CUpdatePITRCommand). `enabled` sets the
// PointInTimeRecoverySpecification.PointInTimeRecoveryEnabled flag. On success
// the executor emits the `ALTER` status token. The SDK-free struct keeps the
// seam AWS-type-free (D11); the CLI adapter translates it to
// Aws::DynamoDB::Model::UpdateContinuousBackupsRequest and composes the
// byte-exact "Setting PITR failed. ..." failure message the original logged
// (D-2e-13). EXPLAIN serializes it under the quirky label
// `UpdateContinuousBackupsRequest(...)` — the ONLY EXPLAIN label the original
// gives a `Request` suffix (D-2e-11).
struct UpdateContinuousBackupsIn {
    std::string table_name;
    bool enabled = false;
};
struct UpdateContinuousBackupsOut {};

// ---- ListTables ------------------------------------------------------------
// A minimal seam op used by the 2a readiness-gate smoke program to prove real
// backend reachability. It is NOT the full SHOW TABLES command (arch §7:
// ListTables + a DescribeTable per table), which lands in its own sub-phase;
// this only surfaces the raw table-name list. Pagination is folded in by the
// adapter (it follows LastEvaluatedTableName), so callers get the full list.
struct ListTablesIn {};
struct ListTablesOut {
    std::vector<std::string> table_names;
};

// ---- DescribeTable ---------------------------------------------------------
// Two consumers share this one DescribeTable view:
//   * the planner, which needs only the key schema to decide GetItem/Query/Scan
//     (arch §8; the original did this via DescribeTable in get_key_schema,
//     arch §10); LSIs are searched before GSIs when resolving a `.index`; and
//   * the SHOW TABLES / DESCRIBE report, which needs the table's status,
//     billing/throughput, table class, identifiers and index counts (arch §7).
// The report fields are populated by the adapter from the SDK TableDescription;
// the planner ignores them. Enum-valued fields are stored as the SDK mapper's
// NAME string (e.g. "ACTIVE", "PROVISIONED") so the engine needs no enum<->name
// tables and the FakeDDBClient can set them directly. `has`-style optionals
// mirror the SDK's *HasBeenSet() so the report reproduces the original's
// defaulting (e.g. the PAY_PER_REQUEST "*" / STANDARD "*" markers, show_tables
// .cpp). Fields the full DESCRIBE report also needs (PITR, autoscaling, the
// per-index detail/stream/sse) land when DESCRIBE ports (next 2b follow-on).
struct TableKeySchema {
    std::string hash_key;   ///< partition (HASH) key attribute name
    std::string range_key;  ///< sort (RANGE) key attribute name ("" if none)
};

/// Warm-throughput description for the DESCRIBE report (table-level and
/// per-GSI). Set only when the SDK reported warm throughput. `status` is the
/// mapped status name ("ACTIVE"/"UPDATING"/...) — the table level maps it via
/// TableStatusMapper, a GSI via IndexStatusMapper, both yielding the same
/// strings (arch §7, describe.cpp __show_table_warm_throughput / __show_gsi).
struct WarmThroughputDescription {
    long long read_units_per_second = 0;
    long long write_units_per_second = 0;
    std::string status;
};

struct SecondaryIndexSchema {
    std::string index_name;
    TableKeySchema key_schema;

    // ---- DESCRIBE report detail -------------------------------------------
    // The planner and SHOW TABLES set only index_name + key_schema; the DESCRIBE
    // adapter fills the rest for a GSI. LSIs leave the GSI-only fields default:
    // the original __show_lsi prints only name + key schema (describe.cpp).
    Projection projection{};  ///< GSI projection (type + non-key attrs)
    bool projection_set = false;
    std::string index_status{};  ///< mapped IndexStatus name ("ACTIVE"/...)
    bool backfilling = false;
    long long provisioned_read_capacity_units = 0;
    long long provisioned_write_capacity_units = 0;
    bool on_demand_throughput_set = false;
    long long on_demand_max_read_request_units = -1;
    long long on_demand_max_write_request_units = -1;
    std::optional<WarmThroughputDescription> warm_throughput{};
};
struct DescribeTableIn {
    std::string table_name;
};
struct DescribeTableOut {
    std::string table_name;
    TableKeySchema key_schema;
    std::vector<SecondaryIndexSchema> local_secondary_indexes;
    std::vector<SecondaryIndexSchema> global_secondary_indexes;

    // ---- Report fields (SHOW TABLES; shared with the coming DESCRIBE) ------
    std::string table_status;  ///< mapped TableStatus name ("ACTIVE", ...)
    std::string table_id;
    std::string table_arn;
    /// Mapped BillingMode name, set iff the SDK BillingModeSummary was present.
    std::optional<std::string> billing_mode_summary;
    /// Whether the SDK ProvisionedThroughput was present (used only when no
    /// BillingModeSummary — the original's fallback to "PROVISIONED").
    bool provisioned_throughput_set = false;
    /// Mapped TableClass name, set iff the SDK TableClassSummary was present
    /// (absent => the report shows "STANDARD*").
    std::optional<std::string> table_class_summary;

    // ---- Full DESCRIBE report fields (arch §7, describe.cpp) --------------
    // Populated by the adapter for DESCRIBE; SHOW TABLES and the planner ignore
    // them. These carry the detail the one-line SHOW TABLES row does not need:
    // attribute definitions, timestamps/sizes, throughput numbers, warm
    // throughput, stream and SSE state (per-index detail lives on the
    // SecondaryIndexSchema entries above).
    std::vector<AttributeDef> attribute_definitions;
    std::string creation_date_time;  ///< ISO 8601 (formatted by the adapter)
    long long table_size_bytes = 0;
    long long item_count = 0;
    bool deletion_protection_enabled = false;
    /// Provisioned throughput numbers (used when billing is PROVISIONED — the
    /// SHOW TABLES fields above carry only the mode name, DESCRIBE needs these).
    long long provisioned_read_capacity_units = 0;
    long long provisioned_write_capacity_units = 0;
    /// On-demand max request units, set iff the SDK OnDemandThroughput was
    /// present (each unset dimension reports -1, as the original did).
    bool on_demand_throughput_set = false;
    long long on_demand_max_read_request_units = -1;
    long long on_demand_max_write_request_units = -1;
    std::optional<WarmThroughputDescription> warm_throughput;
    /// Stream: nullopt => "Stream: Disabled" (spec absent or disabled); a value
    /// => the mapped StreamViewType name (stream enabled), describe.cpp
    /// __show_stream.
    std::optional<std::string> stream_view_type;
    /// SSE: nullopt => "SSE: Not set"; a value => the mapped SSEStatus name
    /// ("ENABLED"/...), describe.cpp __show_table_sse.
    std::optional<std::string> sse_status;
};

// ---- DescribeTimeToLive ----------------------------------------------------
// SHOW TABLES and DESCRIBE pair DescribeTable with DescribeTimeToLive (the
// original CDescribeHelper issues both; a non-brief SHOW TABLES row needs BOTH
// to succeed, else it prints just the table name — show_tables.cpp). `status`
// is the mapped TimeToLiveStatus name ("ENABLED"/"ENABLING"/"DISABLING"/
// "DISABLED"); `attribute_name` is the TTL attribute, shown only when the
// status is ENABLED or ENABLING (arch §7).
struct DescribeTimeToLiveIn {
    std::string table_name;
};
struct TimeToLiveOut {
    std::string status;
    std::string attribute_name;
};

// ---- DescribeContinuousBackups (PITR; DESCRIBE) ----------------------------
// DESCRIBE calls DescribeContinuousBackups to report PITR state (arch §7,
// describe.cpp). The original prints a line only when the status is ENABLED or
// DISABLED, and silently skips the section when the call fails — so the DESCRIBE
// executor tolerates an Error here (it skips the section rather than failing).
struct DescribeContinuousBackupsIn {
    std::string table_name;
};
struct ContinuousBackupsOut {
    /// Mapped PointInTimeRecoveryStatus name ("ENABLED"/"DISABLED"); any other
    /// value => no PITR line (the original printed only on ENABLED/DISABLED).
    std::string pitr_status;
    std::string earliest_restorable_date_time;  ///< ISO 8601, set when ENABLED
    std::string latest_restorable_date_time;    ///< ISO 8601, set when ENABLED
};

// ---- DescribeTableReplicaAutoScaling (DESCRIBE) ----------------------------
// DESCRIBE calls DescribeTableReplicaAutoScaling and prints one line per replica
// region (arch §7, describe.cpp __show_autoscaling). As with PITR, the original
// silently skips the section when the call fails, so the executor tolerates an
// Error here.
struct ReplicaAutoScalingDescription {
    std::string region_name;
    std::string replica_status;  ///< mapped ReplicaStatus name
};
struct DescribeTableReplicaAutoScalingIn {
    std::string table_name;
};
struct TableReplicaAutoScalingOut {
    std::vector<ReplicaAutoScalingDescription> replicas;
};

// ---- TransactGetItems ------------------------------------------------------
// A read transaction (BEGIN; <SELECTs>; COMMIT) issues ONE TransactGetItems
// with one Get per inner SELECT, each reduced to a full-key GetItem (arch §7,
// transact-read.hpp). The engine aliases the projection through a SymbolTable
// (one per inner item, in parse order — D-2d-11), so a Get carries the
// serialized ProjectionExpression + ExpressionAttributeNames; `key` is the
// literal get_gud_key map. A read transaction never carries values.
struct TransactGet {
    std::string table_name;
    ValueMap key;
    std::optional<std::string> projection_expression;
    ExpressionAttributeNames expression_attribute_names;
};
struct TransactGetItemsIn {
    std::vector<TransactGet> items;
};
struct TransactGetItemsOut {
    /// One response per Get, in request order. A missing item is nullopt; the
    /// original rendered every response (including empties) via show_item,
    /// which yields the empty string for an empty item (select_helper.cpp) —
    /// the engine maps nullopt/empty to an empty ItemRow that the Renderer
    /// skips (arch §9). The itxt goldens all hit existing keys, so the
    /// empty-response shape is unpinned; reproduced from the oracle (D-2d §7).
    std::vector<std::optional<ItemRow>> responses;
};

// ---- TransactWriteItems ----------------------------------------------------
// A write transaction (BEGIN; <writes>; COMMIT) issues ONE TransactWriteItems
// with one sub-item per inner write (arch §7, transact-write.hpp). INSERT/
// REPLACE -> Put (one per row); UPDATE/UPSERT -> Update (full key); DELETE ->
// Delete (full key); EXISTS/NOT EXISTS SELECT -> ConditionCheck. No sub-item
// carries ReturnValues (the original never sets them in a transaction —
// D-2d-10). Each sub-item's expression attributes come from a per-item
// SymbolTable (arch §6).
struct TransactPut {
    std::string table_name;
    ValueMap item;
    std::optional<std::string> condition_expression;  ///< INSERT: attribute_not_exists(pk)
    ExpressionAttributeNames expression_attribute_names;
    ValueMap expression_attribute_values;
};
struct TransactUpdate {
    std::string table_name;
    ValueMap key;
    std::string update_expression;
    std::optional<std::string> condition_expression;
    ExpressionAttributeNames expression_attribute_names;
    ValueMap expression_attribute_values;
};
struct TransactDelete {
    std::string table_name;
    ValueMap key;
    std::optional<std::string> condition_expression;
    ExpressionAttributeNames expression_attribute_names;
    ValueMap expression_attribute_values;
};
struct TransactConditionCheck {
    std::string table_name;
    ValueMap key;
    std::string condition_expression;  ///< always present: attribute_[not_]exists(...)
    ExpressionAttributeNames expression_attribute_names;
    ValueMap expression_attribute_values;
};
using TransactWriteItem =
    std::variant<TransactPut, TransactUpdate, TransactDelete, TransactConditionCheck>;
struct TransactWriteItemsIn {
    std::vector<TransactWriteItem> items;
};
struct TransactWriteItemsOut {};

/// A request to serialize for EXPLAIN WITHOUT executing (P2a-03 §4.3). The
/// adapter translates the active alternative to its SDK request and returns the
/// raw SerializePayload() string; the engine never sees an Aws:: type. One
/// alternative per in-scope API the EXPLAIN paths can emit (arch §7/§8).
using ExplainRequest =
    std::variant<GetItemIn, QueryIn, ScanIn, PutItemIn, CreateTableIn, UpdateTableIn,
                 UpdateTimeToLiveIn, UpdateContinuousBackupsIn, DeleteTableIn, DescribeTableIn,
                 DescribeTimeToLiveIn, DescribeContinuousBackupsIn,
                 DescribeTableReplicaAutoScalingIn, UpdateItemIn, DeleteItemIn, TransactGetItemsIn,
                 TransactWriteItemsIn>;

/// The DynamoDB client seam. Borrowed by Session (not owned); the embedder
/// guarantees the client outlives the Session (see P2a-02 §7.2).
class IDDBClient {
public:
    virtual ~IDDBClient() = default;

    virtual Result<GetItemOut> get_item(const GetItemIn&) = 0;
    virtual Result<QueryOut> query(const QueryIn&) = 0;
    virtual Result<ScanOut> scan(const ScanIn&) = 0;
    virtual Result<PutItemOut> put_item(const PutItemIn&) = 0;
    virtual Result<CreateTableOut> create_table(const CreateTableIn&) = 0;

    /// UpdateTable for an ALTER TABLE SET ... (arch §7, the original
    /// CUpdateTableCommand). Sets only the fields the request carries; on
    /// success the executor emits the `ALTER` status token. A service failure
    /// is returned as an Error (the adapter composes the byte-exact
    /// "Update Table failed. ..." message the original logged, D-2e-13).
    virtual Result<UpdateTableOut> update_table(const UpdateTableIn&) = 0;

    /// UpdateTimeToLive for an ALTER TABLE SET TTL ... (arch §7, the original
    /// CUpdateTableTTLCommand). The executor probes DescribeTimeToLive first,
    /// then calls this (skipping it when disabling an already-disabled table —
    /// D-2e-7). A service failure is returned as an Error (the adapter composes
    /// the byte-exact "Error updating TTL. ..." message, D-2e-13).
    virtual Result<UpdateTimeToLiveOut> update_time_to_live(const UpdateTimeToLiveIn&) = 0;

    /// UpdateContinuousBackups for an ALTER TABLE SET PITR ... (arch §7, the
    /// original CUpdatePITRCommand). On success the executor emits the `ALTER`
    /// status token. A service failure is returned as an Error (the adapter
    /// composes the byte-exact "Setting PITR failed. ..." message, D-2e-13).
    virtual Result<UpdateContinuousBackupsOut> update_continuous_backups(
        const UpdateContinuousBackupsIn&) = 0;

    virtual Result<DeleteTableOut> delete_table(const DeleteTableIn&) = 0;
    virtual Result<ListTablesOut> list_tables(const ListTablesIn&) = 0;
    virtual Result<DescribeTableOut> describe_table(const DescribeTableIn&) = 0;

    /// UpdateItem for a single addressed item (UPDATE/UPSERT; arch §7). A
    /// ConditionalCheckFailedException is mapped by the adapter to
    /// `Error{Errc::conditional_check_failed, "The conditional request failed"}`
    /// so the UPDATE executor can intercept it and count it as a ccf (D-2c-7)
    /// rather than aborting; any other service error maps to Errc::service_error.
    virtual Result<UpdateItemOut> update_item(const UpdateItemIn&) = 0;

    /// DeleteItem for a single addressed item (DELETE; arch §7). Same ccf
    /// mapping as update_item (D-2c-7).
    virtual Result<DeleteItemOut> delete_item(const DeleteItemIn&) = 0;

    /// TransactGetItems for a read transaction (arch §7, transact-read.hpp). A
    /// service failure (e.g. TransactionCanceledException) is mapped by the
    /// adapter to an Error whose message is the byte-exact
    /// "Transaction failed. <ExceptionName>. <RequestId>. <Message>" string the
    /// original logged (D-2d-9); the transaction executor emits `ABORT` then
    /// returns that Error (exit nonzero, D-2d-5 path 2).
    virtual Result<TransactGetItemsOut> transact_get_items(const TransactGetItemsIn&) = 0;

    /// TransactWriteItems for a write transaction (arch §7, transact-write.hpp).
    /// Same failure mapping as transact_get_items (D-2d-9): a cancellation
    /// yields the composed "Transaction failed. ..." Error. Success commits
    /// (the executor emits the `COMMIT` token, D-2d-8).
    virtual Result<TransactWriteItemsOut> transact_write_items(const TransactWriteItemsIn&) = 0;

    /// DescribeTimeToLive for a table (arch §7/§10). SHOW TABLES / DESCRIBE pair
    /// it with describe_table; a failure here makes a non-brief SHOW TABLES row
    /// collapse to just the table name (the original's has_ttl() == false path).
    virtual Result<TimeToLiveOut> describe_time_to_live(const DescribeTimeToLiveIn&) = 0;

    /// DescribeContinuousBackups for a table (PITR state; arch §7). Used only by
    /// DESCRIBE. The DESCRIBE executor skips the PITR line when this fails, so a
    /// returned Error is non-fatal to the command (matching the original's
    /// IsSuccess() guard).
    virtual Result<ContinuousBackupsOut> describe_continuous_backups(
        const DescribeContinuousBackupsIn&) = 0;

    /// DescribeTableReplicaAutoScaling for a table (arch §7). Used only by
    /// DESCRIBE. As with PITR, the DESCRIBE executor skips the replica section
    /// when this fails, so a returned Error is non-fatal to the command.
    virtual Result<TableReplicaAutoScalingOut> describe_table_replica_auto_scaling(
        const DescribeTableReplicaAutoScalingIn&) = 0;

    /// Block (bounded exponential backoff, 100ms doubling to a 2s cap, arch
    /// §10) until `table_name` reaches ACTIVE. Used by CREATE TABLE unless
    /// NOWAIT. Returns success when the table is ACTIVE, or an Error on a
    /// terminal control-plane failure (a non-retryable error, or a state that
    /// cannot progress to ACTIVE). The waiter lives behind the seam because it
    /// owns the SDK's error-classification/polling semantics; the
    /// FakeDDBClient returns immediately.
    virtual Result<void> wait_for_table_active(const std::string& table_name) = 0;

    /// Block (bounded exponential backoff, 100ms doubling to a 2s cap, arch
    /// §10) until `table_name` no longer exists. Used by DROP TABLE unless
    /// NOWAIT. Returns success when the table is gone, or an Error on a
    /// terminal control-plane failure. The waiter lives behind the seam
    /// because it owns the SDK's error-classification/polling semantics; the
    /// FakeDDBClient returns immediately.
    virtual Result<void> wait_for_table_gone(const std::string& table_name) = 0;
    // update/transact + a CancelToken: added per sub-phase (2b-2d).

    /// Serialize a planned request to its EXPLAIN body — the SDK's
    /// SerializePayload() output (tab-indented JSON) — WITHOUT executing it.
    /// Used only by the EXPLAIN paths; the serialization itself makes no service
    /// call (note EXPLAIN SELECT/INSERT still resolve the key schema via
    /// describe_table BEFORE building the request — arch §8). The Renderer
    /// applies the explain_string/strip_newlines transform, so this returns the
    /// raw body (P2a-03 §4.3). This is the sole EXPLAIN seam; the engine's
    /// executors wrap the body in `ApiName(...)` and pick the format.
    virtual Result<std::string> explain_request(const ExplainRequest&) = 0;
};

}  // namespace ddbsh

#endif  // DDBSH_IDDBCLIENT_HPP

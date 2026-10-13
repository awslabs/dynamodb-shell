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
struct SecondaryIndexSchema {
    std::string index_name;
    TableKeySchema key_schema;
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
    virtual Result<DeleteTableOut> delete_table(const DeleteTableIn&) = 0;
    virtual Result<ListTablesOut> list_tables(const ListTablesIn&) = 0;
    virtual Result<DescribeTableOut> describe_table(const DescribeTableIn&) = 0;

    /// DescribeTimeToLive for a table (arch §7/§10). SHOW TABLES / DESCRIBE pair
    /// it with describe_table; a failure here makes a non-brief SHOW TABLES row
    /// collapse to just the table name (the original's has_ttl() == false path).
    virtual Result<TimeToLiveOut> describe_time_to_live(const DescribeTimeToLiveIn&) = 0;

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
};

}  // namespace ddbsh

#endif  // DDBSH_IDDBCLIENT_HPP

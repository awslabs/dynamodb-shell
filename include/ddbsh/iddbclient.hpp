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

// ---- DescribeTable (key-schema resolution) ---------------------------------
// The planner resolves a table's (or secondary index's) key schema to decide
// GetItem/Query/Scan (arch §8); the original did this via DescribeTable in
// get_key_schema (arch §10). This SDK-free view carries only what the planner
// needs now — the full DESCRIBE / SHOW TABLES report fields (status, throughput,
// item count, TTL, PITR, ...) are added when those commands port (2b follow-on).
// LSIs are searched before GSIs when resolving a `.index` (arch §10).
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
    virtual Result<ListTablesOut> list_tables(const ListTablesIn&) = 0;
    virtual Result<DescribeTableOut> describe_table(const DescribeTableIn&) = 0;
    // update/delete/transact/create/drop/backup/pitr + CancelToken:
    // added per sub-phase (2b-2d).
};

}  // namespace ddbsh

#endif  // DDBSH_IDDBCLIENT_HPP

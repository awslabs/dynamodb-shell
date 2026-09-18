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
#include <vector>

#include "ddbsh/result.hpp"
#include "ddbsh/value.hpp"

namespace ddbsh {

/// A single returned row: attribute name -> Value (source order; Renderer
/// sorts).
struct ItemRow {
    ValueMap attributes;
};

// ---- GetItem ---------------------------------------------------------------
struct GetItemIn {
    std::string table_name;
    ValueMap key;
    std::vector<std::string> projection;
    bool consistent_read = false;
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
    std::vector<std::pair<std::string, std::string>> expression_attribute_names;
    ValueMap expression_attribute_values;
    std::vector<std::string> projection;
    bool consistent_read = false;
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
    std::vector<std::pair<std::string, std::string>> expression_attribute_names;
    ValueMap expression_attribute_values;
    std::vector<std::string> projection;
    bool consistent_read = false;
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
    // update/delete/transact/describe/create/drop/backup/pitr + CancelToken:
    // added per sub-phase (2b-2d).
};

}  // namespace ddbsh

#endif  // DDBSH_IDDBCLIENT_HPP

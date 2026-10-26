// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// write_common.hpp — shared helpers for the UPDATE/UPSERT and DELETE executors.
//
// PRIVATE engine header (not installed). SDK-free helpers common to the two
// mutating-command executors (update_executor / delete_executor):
//   * the SymbolTable -> seam flatten helpers (names/values/key), identical to
//     the ones the SELECT planner uses;
//   * the bulk-enumeration read request builder (the Query/Scan that lists the
//     base-table keys of the items to mutate), a faithful port of the original
//     CSelectHelper::setup(table,index,where,ratelimit) + query_request /
//     scan_request path (select_helper.cpp): the ProjectionExpression is the
//     BASE-TABLE key (pk, and rk if present) so the per-item write can address
//     the base table even when the read runs on an index (D-2c-9); the key
//     condition / filter is evaluated against the INDEX key schema when an index
//     is targeted and the base-table schema otherwise; ConsistentRead is false;
//     ReturnConsumedCapacity is TOTAL iff a ratelimit is present (D-2c-14).
//
// The enumeration read's SymbolTable is minted from the read prefix, which the
// executor obtains AFTER the write-request prefix (D-2c-10, the hard
// byte-fidelity ordering pinned by iut-3 / iut-25).

#ifndef DDBSH_SRC_EXECUTE_WRITE_COMMON_HPP
#define DDBSH_SRC_EXECUTE_WRITE_COMMON_HPP

#include <optional>
#include <string>
#include <vector>

#include "ddbsh/iddbclient.hpp"
#include "expressions/logical_expression.hpp"
#include "expressions/symbol_table.hpp"
#include "expressions/where.hpp"
#include "parser/ast.hpp"

namespace ddbsh::engine {

/// Map the parser's ReturnValue (engine::ReturnValue) to the client seam's
/// public ddbsh::ReturnValue. The two enums are intentionally distinct (the
/// seam is SDK-free public API, the AST is engine-internal); a scoped-enum
/// static_cast between them is ill-formed, so map explicitly.
inline ddbsh::ReturnValue to_client_return_value(engine::ReturnValue rv) {
    switch (rv) {
        case engine::ReturnValue::all_old:
            return ddbsh::ReturnValue::all_old;
        case engine::ReturnValue::all_new:
            return ddbsh::ReturnValue::all_new;
        case engine::ReturnValue::updated_old:
            return ddbsh::ReturnValue::updated_old;
        case engine::ReturnValue::updated_new:
            return ddbsh::ReturnValue::updated_new;
        case engine::ReturnValue::none:
            break;
    }
    return ddbsh::ReturnValue::none;
}

/// Which enumeration API the bulk write path chose.
enum class WriteReadApi { query, scan };

/// The planned bulk-enumeration read. Exactly one member is populated per `api`.
struct WriteReadPlan {
    WriteReadApi api = WriteReadApi::scan;
    QueryIn query;
    ScanIn scan;
};

/// ExpressionAttributeNames (token -> name, ordered) from a SymbolTable.
inline ExpressionAttributeNames names_of(const SymbolTable& st) {
    ExpressionAttributeNames names;
    for (auto& [token, name] : st.get_names()) {
        names.emplace_back(token, name);
    }
    return names;
}

/// ExpressionAttributeValues (token -> Value, ordered) from a SymbolTable.
inline ValueMap values_of(const SymbolTable& st) {
    ValueMap values;
    for (const auto& [token, value] : st.get_values()) {
        values.emplace_back(token, value);
    }
    return values;
}

/// A GetItem/Update/Delete key map (std::map, sorted by name) flattened to the
/// seam's ValueMap, matching the SDK's Key serialization order.
inline ValueMap key_of(const KeyMap& key) {
    ValueMap out;
    for (const auto& [name, value] : key) {
        out.emplace_back(name, value);
    }
    return out;
}

/// Project the base-table key attributes through the read SymbolTable, exactly
/// as CSelectHelper::serialize_projection: `#a<rp>1[, #a<rp>2]`.
inline std::string project_base_keys(const std::string& base_pk, const std::string& base_rk,
                                     SymbolTable& st) {
    std::string projection = st.new_attribute(base_pk);
    if (!base_rk.empty()) {
        projection += ", " + st.new_attribute(base_rk);
    }
    return projection;
}

/// Build the bulk-enumeration Query/Scan request (port of
/// CSelectHelper::query_request / scan_request for the write path). A present
/// `qplan` (from Where::plan_query against the key schema the WHERE is
/// evaluated against — the index schema when an index is targeted, base
/// otherwise) selects Query; its absence selects Scan. `base_pk/base_rk` is the
/// base-table key used for the projection so the per-item write can address the
/// base table even when the read runs on an index (D-2c-9). `st` is the read
/// SymbolTable (minted from the read prefix). The minting order — projection,
/// then key condition, then filter — matches the original exactly
/// (byte-fidelity, iut-3 / iut-25).
inline WriteReadPlan build_write_read(const std::string& table,
                                      const std::optional<std::string>& index, const Where* where,
                                      const std::optional<QueryKeyPlan>& qplan,
                                      const std::string& base_pk, const std::string& base_rk,
                                      bool ratelimit, SymbolTable& st) {
    const ddbsh::ReturnConsumedCapacity rcc =
        ratelimit ? ddbsh::ReturnConsumedCapacity::total : ddbsh::ReturnConsumedCapacity::none;

    WriteReadPlan plan;
    if (qplan.has_value()) {
        plan.api = WriteReadApi::query;
        QueryIn& r = plan.query;
        r.table_name = table;
        if (index.has_value()) {
            r.index_name = index;
        }
        r.consistent_read = false;
        r.return_consumed_capacity = rcc;
        r.projection_expression = project_base_keys(base_pk, base_rk, st);
        r.key_condition_expression = where->query_key_condition_expression(*qplan, &st);
        std::string filter = where->query_filter_expression(*qplan, &st);
        if (!filter.empty()) {
            r.filter_expression = std::move(filter);
        }
        r.expression_attribute_names = names_of(st);
        r.expression_attribute_values = values_of(st);
        return plan;
    }

    plan.api = WriteReadApi::scan;
    ScanIn& r = plan.scan;
    r.table_name = table;
    if (index.has_value()) {
        r.index_name = index;
    }
    r.consistent_read = false;
    r.return_consumed_capacity = rcc;
    r.projection_expression = project_base_keys(base_pk, base_rk, st);
    if (where != nullptr) {
        std::string filter = where->scan_filter_expression(&st);
        if (!filter.empty()) {
            r.filter_expression = std::move(filter);
        }
    }
    r.expression_attribute_names = names_of(st);
    r.expression_attribute_values = values_of(st);
    return plan;
}

}  // namespace ddbsh::engine

#endif  // DDBSH_SRC_EXECUTE_WRITE_COMMON_HPP

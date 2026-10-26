// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// select_planner.cpp — see select_planner.hpp.

#include "planner/select_planner.hpp"

#include <optional>
#include <utility>
#include <vector>

#include "expressions/symbol_table.hpp"
#include "expressions/where.hpp"
#include "planner/get_key_schema.hpp"

namespace ddbsh::engine {

namespace {

/// Map the parser's ReturnConsumedCapacity to the client seam's enum. Both are
/// named ReturnConsumedCapacity (ddbsh::engine:: vs the public ddbsh::), so the
/// client-seam type is fully qualified throughout the planner to disambiguate.
ddbsh::ReturnConsumedCapacity to_client_rcc(engine::ReturnConsumedCapacity rcc) {
    switch (rcc) {
        case engine::ReturnConsumedCapacity::total:
            return ddbsh::ReturnConsumedCapacity::total;
        case engine::ReturnConsumedCapacity::indexes:
            return ddbsh::ReturnConsumedCapacity::indexes;
        case engine::ReturnConsumedCapacity::none:
            break;
    }
    return ddbsh::ReturnConsumedCapacity::none;
}

/// Serialize the projection list through the shared symbol table, aliasing
/// each attribute path (dotted/subscripted paths are tokenized by the symbol
/// table). Mirrors CSelectHelper::serialize_projection(&st): `#a..1, #a..2`.
std::string serialize_projection(const std::vector<std::string>& projection, SymbolTable& st) {
    std::string out;
    for (const auto& attr : projection) {
        if (out.empty()) {
            out = st.new_attribute(attr);
        } else {
            out += ", " + st.new_attribute(attr);
        }
    }
    return out;
}

/// token -> name map (ordered) flattened to the seam's ordered vector form.
ExpressionAttributeNames names_of(const SymbolTable& st) {
    ExpressionAttributeNames names;
    for (auto& [token, name] : st.get_names()) {
        names.emplace_back(token, name);
    }
    return names;
}

/// token -> Value map (ordered) flattened to a ValueMap in the same order.
ValueMap values_of(const SymbolTable& st) {
    ValueMap values;
    for (const auto& [token, value] : st.get_values()) {
        values.emplace_back(token, value);
    }
    return values;
}

/// A GetItem/Update/Delete key map flattened to the seam's ValueMap. KeyMap is
/// a std::map (sorted by attribute name), matching the SDK's Key serialization
/// order the original emitted.
ValueMap key_of(const KeyMap& key) {
    ValueMap out;
    for (const auto& [name, value] : key) {
        out.emplace_back(name, value);
    }
    return out;
}

}  // namespace

// Build a SearchVectors plan for a SELECT on a vector index (phase 4b). The
// WHERE tree becomes the SearchConditionExpression and the projection the
// ProjectionExpression, aliased through ONE SymbolTable in projection-then-
// filter order (the house convention shared with Query/Scan) for
// byte-deterministic EXPLAIN. Error cases follow the P4b design §7 (D-4b).
static Result<SelectPlan> plan_search_vectors(const SelectCmd& cmd, const VectorIndexSchema& vi,
                                              const std::string& alias_prefix) {
    if (!cmd.search_vector.has_value()) {
        return fail(Errc::invalid_argument, "A SELECT on vector index " + vi.index_name +
                                                " requires a NEAREST <n> TO [ ... ] clause.");
    }
    if (cmd.consistent) {
        return fail(Errc::invalid_argument, "CONSISTENT is not supported with vector search.");
    }
    const int topk = cmd.topk.value_or(0);
    if (topk <= 0) {
        return fail(Errc::invalid_argument, "Vector search TopK must be a positive integer.");
    }
    const Value& qv = *cmd.search_vector;
    if (qv.type != Value::Type::L) {
        return fail(Errc::invalid_argument, "The NEAREST search vector must be a numeric list.");
    }
    for (const auto& e : qv.list) {
        if (e.type != Value::Type::N) {
            return fail(Errc::invalid_argument,
                        "The NEAREST search vector must contain only numbers.");
        }
    }
    if (vi.dimensions > 0 && static_cast<long long>(qv.list.size()) != vi.dimensions) {
        return fail(Errc::invalid_argument, "The search vector has " +
                                                std::to_string(qv.list.size()) +
                                                " values; vector index " + vi.index_name +
                                                " expects " + std::to_string(vi.dimensions) + ".");
    }

    SymbolTable st(alias_prefix);
    SelectPlan plan;
    plan.api = SelectApi::search_vectors;
    SearchVectorsIn& r = plan.search_vectors;
    r.table_name = cmd.table;
    r.index_name = *cmd.index;
    r.search_vector = qv.list;
    r.topk = topk;
    if (!cmd.projection.empty()) {
        r.projection_expression = serialize_projection(cmd.projection, st);
    }
    if (cmd.where != nullptr) {
        std::string filter = cmd.where->scan_filter_expression(&st);
        if (!filter.empty()) {
            r.search_condition_expression = std::move(filter);
        }
    }
    r.expression_attribute_names = names_of(st);
    r.expression_attribute_values = values_of(st);
    r.return_consumed_capacity =
        cmd.ratelimit.has_value() ? ddbsh::ReturnConsumedCapacity::total : to_client_rcc(cmd.rcc);
    return plan;
}

Result<SelectPlan> plan_select(const SelectCmd& cmd, IDDBClient& client,
                               const std::string& alias_prefix) {
    // Vector-index routing (phase 4b) is evaluated FIRST. A `.index` that
    // DescribeTable reports as a vector index routes to SearchVectors; a
    // NEAREST clause on a non-vector target is a semantic error.
    if (cmd.index.has_value()) {
        auto described = client.describe_table(DescribeTableIn{cmd.table});
        if (!described.has_value()) {
            return fail(described.error());
        }
        for (const auto& vi : described->vector_indexes) {
            if (vi.index_name == *cmd.index) {
                return plan_search_vectors(cmd, vi, alias_prefix);
            }
        }
        // Not a vector index -> fall through to the classic GetItem/Query/Scan
        // path (which re-resolves the key schema via get_key_schema).
    }
    if (cmd.search_vector.has_value()) {
        return fail(Errc::invalid_argument,
                    cmd.index.has_value()
                        ? *cmd.index + " is not a vector index; NEAREST requires a vector index."
                        : "Vector search (NEAREST) requires a vector index (table.vectorindex).");
    }

    auto resolved = get_key_schema(client, cmd.table, cmd.index.value_or(""));
    if (!resolved.has_value()) {
        return fail(resolved.error());
    }
    const std::vector<std::string>& pk = resolved->pk;
    const std::vector<std::string>& rk = resolved->rk;

    const bool has_index = cmd.index.has_value();
    const Where* where = cmd.where.get();

    // Access-pattern probes, in the original's order (select_helper.cpp
    // can_getitem/can_query). GetItem is base-table only (indexes never
    // GetItem). A SELECT with no WHERE falls through to Scan. plan_query is the
    // generalized Query probe (phase 4a): its presence selects Query (on the
    // base table or the index), its absence falls through to Scan.
    const bool can_getitem = !has_index && where != nullptr && where->can_getitem(pk, rk);
    const std::optional<QueryKeyPlan> qplan =
        (where != nullptr) ? where->plan_query(pk, rk) : std::nullopt;

    // One symbol table per SELECT (matching one CSymbolTable per request). The
    // aliasing call order — projection, then key condition, then filter —
    // governs the token numbering and must match the original exactly.
    SymbolTable st(alias_prefix);
    SelectPlan plan;

    if (can_getitem) {
        plan.api = SelectApi::get_item;
        GetItemIn& r = plan.get_item;
        r.table_name = cmd.table;
        r.consistent_read = cmd.consistent;
        if (!cmd.projection.empty()) {
            r.projection_expression = serialize_projection(cmd.projection, st);
        }
        r.expression_attribute_names = names_of(st);
        // GetItem ignores ratelimit (there is nothing to paginate); the user's
        // RETURN CONSUMED CAPACITY still flows through.
        r.return_consumed_capacity = to_client_rcc(cmd.rcc);
        r.key = key_of(where->get_gud_key(pk, rk));
        return plan;
    }

    // A ratelimit forces TOTAL so paginated reads can be metered (arch §8).
    const ddbsh::ReturnConsumedCapacity rcc =
        cmd.ratelimit.has_value() ? ddbsh::ReturnConsumedCapacity::total : to_client_rcc(cmd.rcc);

    if (qplan.has_value()) {
        plan.api = SelectApi::query;
        QueryIn& r = plan.query;
        r.table_name = cmd.table;
        if (has_index) {
            r.index_name = cmd.index;
        }
        r.consistent_read = cmd.consistent;
        r.return_consumed_capacity = rcc;
        if (!cmd.projection.empty()) {
            r.projection_expression = serialize_projection(cmd.projection, st);
        }
        r.key_condition_expression = where->query_key_condition_expression(*qplan, &st);
        std::string filter = where->query_filter_expression(*qplan, &st);
        if (!filter.empty()) {
            r.filter_expression = std::move(filter);
        }
        r.expression_attribute_names = names_of(st);
        r.expression_attribute_values = values_of(st);
        return plan;
    }

    plan.api = SelectApi::scan;
    ScanIn& r = plan.scan;
    r.table_name = cmd.table;
    if (has_index) {
        r.index_name = cmd.index;
    }
    r.consistent_read = cmd.consistent;
    r.return_consumed_capacity = rcc;
    if (!cmd.projection.empty()) {
        r.projection_expression = serialize_projection(cmd.projection, st);
    }
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

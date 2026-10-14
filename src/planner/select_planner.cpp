// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// select_planner.cpp — see select_planner.hpp.

#include "planner/select_planner.hpp"

#include <utility>

#include "expressions/symbol_table.hpp"
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

Result<SelectPlan> plan_select(const SelectCmd& cmd, IDDBClient& client,
                               const std::string& alias_prefix) {
    auto resolved = get_key_schema(client, cmd.table, cmd.index.value_or(""));
    if (!resolved.has_value()) {
        return fail(resolved.error());
    }
    const std::string& pk = resolved->pk;
    const std::string& rk = resolved->rk;

    const bool has_index = cmd.index.has_value();
    const Where* where = cmd.where.get();

    // Access-pattern probes, in the original's order (select_helper.cpp
    // can_getitem/can_query). A SELECT with no WHERE falls through to Scan.
    const bool can_getitem = !has_index && where != nullptr && where->can_getitem(pk, rk);
    const bool can_query = where != nullptr && ((!has_index && where->can_query_table(pk, rk)) ||
                                                (has_index && where->can_query_index(pk, rk)));

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

    if (can_query) {
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
        r.key_condition_expression = where->query_key_condition_expression(pk, rk, &st);
        std::string filter = where->query_filter_expression(pk, rk, &st);
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

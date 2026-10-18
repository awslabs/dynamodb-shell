// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// transact_build.cpp — see transact_build.hpp.

#include "execute/transact_build.hpp"

#include <string>
#include <utility>

#include "execute/delete_executor.hpp"
#include "execute/insert_executor.hpp"
#include "execute/update_executor.hpp"
#include "execute/write_common.hpp"
#include "expressions/symbol_table.hpp"
#include "expressions/where.hpp"
#include "planner/get_key_schema.hpp"

namespace ddbsh::engine {

namespace {

/// Project the SELECT's projection list through the symbol table, exactly as
/// CSelectHelper::serialize_projection(&st): `#a..1[, #a..2]`. Empty list => no
/// ProjectionExpression (select *).
std::string serialize_projection(const std::vector<std::string>& projection, SymbolTable& st) {
    std::string out;
    for (const auto& attr : projection) {
        out += (out.empty() ? "" : ", ") + st.new_attribute(attr);
    }
    return out;
}

}  // namespace

Result<TransactGet> build_transact_get(const SelectCmd& cmd, IDDBClient& client,
                                       const std::function<std::string()>& next_prefix) {
    // A transaction Get is base-table only (the original's can_getitem requires
    // an empty index name). Resolve the base-table key schema.
    auto schema = get_key_schema(client, cmd.table, "");
    if (!schema.has_value()) {
        return fail(schema.error());  // hard error (e.g. table not found)
    }
    const std::string& pk = schema->pk;
    const std::string& rk = schema->rk;
    const Where* where = cmd.where.get();

    if (cmd.index.has_value() || where == nullptr || !where->can_getitem(pk, rk)) {
        return fail(Errc::not_reducible_to_getitem, kSelectNotReducibleMessage);
    }

    // Reducible: mint one prefix (matching the original's per-txget CSymbolTable).
    SymbolTable st(next_prefix());
    TransactGet get;
    get.table_name = cmd.table;
    if (!cmd.projection.empty()) {
        get.projection_expression = serialize_projection(cmd.projection, st);
    }
    get.expression_attribute_names = names_of(st);
    get.key = key_of(where->get_gud_key(pk, rk));
    return get;
}

Result<std::vector<TransactPut>> build_transact_puts(
    const InsertCmd& cmd, IDDBClient& client, const std::function<std::string()>& next_prefix) {
    auto schema = get_key_schema(client, cmd.table_name, "");
    if (!schema.has_value()) {
        return fail(schema.error());
    }
    const std::string& pk = schema->pk;

    if (auto valid = validate_insert_rows(cmd); !valid.has_value()) {
        return fail(valid.error());
    }

    std::vector<TransactPut> puts;
    puts.reserve(cmd.rows.size());
    for (std::size_t row = 0; row < cmd.rows.size(); ++row) {
        PutItemIn req = make_put_request(cmd, row, pk, next_prefix);
        TransactPut put;
        put.table_name = std::move(req.table_name);
        put.item = std::move(req.item);
        put.condition_expression = std::move(req.condition_expression);
        put.expression_attribute_names = std::move(req.expression_attribute_names);
        put.expression_attribute_values = std::move(req.expression_attribute_values);
        puts.push_back(std::move(put));
    }
    return puts;
}

Result<TransactUpdate> build_transact_update(const UpdateCmd& cmd, IDDBClient& client,
                                             const std::function<std::string()>& next_prefix) {
    auto schema = get_key_schema(client, cmd.table, "");
    if (!schema.has_value()) {
        return fail(schema.error());
    }
    const std::string& pk = schema->pk;
    const std::string& rk = schema->rk;
    const Where* where = cmd.where.get();

    if (where == nullptr || !where->can_update_item(pk, rk)) {
        // The original update.cpp txwrite() returns NULL with no log message.
        return fail(Errc::not_reducible_to_getitem, "");
    }

    SymbolTable st(next_prefix());
    UpdateItemIn req = make_update_request(cmd, pk, rk, st);
    TransactUpdate update;
    update.table_name = std::move(req.table_name);
    update.key = key_of(where->get_gud_key(pk, rk));
    update.update_expression = std::move(req.update_expression);
    update.condition_expression = std::move(req.condition_expression);
    update.expression_attribute_names = std::move(req.expression_attribute_names);
    update.expression_attribute_values = std::move(req.expression_attribute_values);
    // No ReturnValues / ReturnConsumedCapacity in a transaction (D-2d-10).
    return update;
}

Result<TransactDelete> build_transact_delete(const DeleteCmd& cmd, IDDBClient& client,
                                             const std::function<std::string()>& next_prefix) {
    auto schema = get_key_schema(client, cmd.table, "");
    if (!schema.has_value()) {
        return fail(schema.error());
    }
    const std::string& pk = schema->pk;
    const std::string& rk = schema->rk;
    const Where* where = cmd.where.get();

    if (where == nullptr || !where->can_delete_item(pk, rk)) {
        return fail(Errc::not_reducible_to_getitem, "");
    }

    SymbolTable st(next_prefix());
    DeleteItemIn req = make_delete_request(cmd, pk, rk, st);
    TransactDelete del;
    del.table_name = std::move(req.table_name);
    del.key = key_of(where->get_gud_key(pk, rk));
    del.condition_expression = std::move(req.condition_expression);
    del.expression_attribute_names = std::move(req.expression_attribute_names);
    del.expression_attribute_values = std::move(req.expression_attribute_values);
    return del;
}

Result<TransactConditionCheck> build_transact_condition_check(
    const SelectCmd& cmd, IDDBClient& client, const std::function<std::string()>& next_prefix) {
    std::string check_func;
    if (cmd.exists_mode == ExistsMode::exists) {
        check_func = "attribute_exists";
    } else if (cmd.exists_mode == ExistsMode::not_exists) {
        check_func = "attribute_not_exists";
    } else {
        // Unreachable via the grammar (a bare SELECT in a write body is a
        // syntax error); reproduced for completeness (D-2d-6).
        return fail(Errc::not_reducible_to_getitem, kSelectNoExistsMessage);
    }

    auto schema = get_key_schema(client, cmd.table, "");
    if (!schema.has_value()) {
        return fail(schema.error());
    }
    const std::string& pk = schema->pk;
    const std::string& rk = schema->rk;
    const Where* where = cmd.where.get();
    if (where == nullptr) {
        // The original would dereference a null where here; we abort safely
        // (no WHERE => no full key to identify the item). No golden reaches it.
        return fail(Errc::not_reducible_to_getitem, "");
    }

    TransactConditionCheck cc;
    cc.table_name = cmd.table;
    cc.key = key_of(where->get_gud_key(pk, rk));

    // Build the condition exactly as select_helper.cpp txwrite: the residual
    // filter, then ` AND `, then check_func() on either the pk (projection `*`)
    // or each projected attribute (concatenated with NO separator — the
    // original's quirk).
    SymbolTable st(next_prefix());
    std::string filter = where->query_filter_expression(pk, rk, &st);
    if (!filter.empty()) {
        filter += " AND ";
    }
    if (cmd.projection.empty()) {
        filter += check_func + "(" + st.new_attribute(pk) + ")";
    } else {
        for (const auto& p : cmd.projection) {
            filter += check_func + "(" + st.new_attribute(p) + ")";
        }
    }
    cc.condition_expression = std::move(filter);
    cc.expression_attribute_names = names_of(st);
    cc.expression_attribute_values = values_of(st);
    return cc;
}

}  // namespace ddbsh::engine

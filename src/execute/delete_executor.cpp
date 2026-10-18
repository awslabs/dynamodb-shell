// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// delete_executor.cpp — see delete_executor.hpp.

#include "execute/delete_executor.hpp"

#include <utility>

#include "execute/write_common.hpp"
#include "expressions/symbol_table.hpp"
#include "planner/get_key_schema.hpp"

namespace ddbsh::engine {

namespace {

/// Enumerated row (base-table keys) -> sorted item key (SDK std::map order).
ValueMap row_key(const ItemRow& item) {
    KeyMap sorted;
    for (const auto& [name, value] : item.attributes) {
        sorted.emplace(name, value);
    }
    return key_of(sorted);
}

}  // namespace

DeleteItemIn make_delete_request(const DeleteCmd& cmd, const std::string& pk, const std::string& rk,
                                 SymbolTable& st) {
    DeleteItemIn request;
    request.table_name = cmd.table;
    request.return_values = to_client_return_value(cmd.return_value);

    if (cmd.where) {
        std::string cond = cmd.where->update_delete_condition_check(pk, rk, &st);
        if (!cond.empty()) {
            request.condition_expression = std::move(cond);
        }
    }

    request.expression_attribute_names = names_of(st);
    request.expression_attribute_values = values_of(st);
    if (cmd.ratelimit.has_value()) {
        request.return_consumed_capacity = ddbsh::ReturnConsumedCapacity::total;
    }
    return request;
}

Result<void> execute_delete(const DeleteCmd& cmd, IDDBClient& client,
                            const std::function<std::string()>& next_prefix, bool explain,
                            OutcomeSink& sink) {
    auto base = get_key_schema(client, cmd.table, "");
    if (!base.has_value()) {
        return fail(base.error());
    }
    const std::string base_pk = base->pk;
    const std::string base_rk = base->rk;
    const Where* where = cmd.where.get();

    // Write template FIRST (first alias prefix), per D-2c-10.
    SymbolTable write_st(next_prefix());
    DeleteItemIn write_template = make_delete_request(cmd, base_pk, base_rk, write_st);

    // ---- single conditional delete (full base-table key) -------------------
    if (where != nullptr && where->can_delete_item(base_pk, base_rk)) {
        DeleteItemIn request = write_template;
        request.key = key_of(where->get_gud_key(base_pk, base_rk));
        if (explain) {
            auto body = client.explain_request(request);
            if (!body.has_value()) {
                return fail(body.error());
            }
            ExplainOutcome out;
            out.blocks.push_back(
                ExplainBlock{"DeleteItem", std::move(*body), ExplainBlock::Format::pretty});
            sink.emit(ExecOutcome{std::move(out)});
            return {};
        }
        auto result = client.delete_item(request);
        BulkWriteOutcome acct{BulkWriteOutcome::Verb::del, 0, 0, 0, std::nullopt};
        if (result.has_value()) {
            acct.modified = 1;
        } else if (result.error().code == Errc::conditional_check_failed) {
            acct.ccf = 1;
        } else {
            return fail(result.error());
        }
        sink.emit(ExecOutcome{acct});
        return {};
    }

    // ---- bulk enumeration --------------------------------------------------
    std::string key_pk = base_pk;
    std::string key_rk = base_rk;
    bool is_query = false;
    if (!cmd.index.has_value()) {
        is_query = where != nullptr && where->can_query_table(base_pk, base_rk);
    } else {
        auto idx = get_key_schema(client, cmd.table, *cmd.index);
        if (!idx.has_value()) {
            return fail(idx.error());
        }
        key_pk = idx->pk;
        key_rk = idx->rk;
        is_query = where != nullptr && where->can_query_index(key_pk, key_rk);
    }

    SymbolTable read_st(next_prefix());
    WriteReadPlan read = build_write_read(cmd.table, cmd.index, where, is_query, key_pk, key_rk,
                                          base_pk, base_rk, cmd.ratelimit.has_value(), read_st);

    ExplainOutcome explain_out;
    BulkWriteOutcome acct{BulkWriteOutcome::Verb::del, 0, 0, 0, std::nullopt};

    // EXPLAIN DELETE via QUERY: emit the Query block ONLY and do not run it
    // (delete.cpp do_query guards the loop behind !explaining()). This is the
    // reproduced asymmetry (§8.3).
    if (explain && read.api == WriteReadApi::query) {
        auto body = client.explain_request(read.query);
        if (!body.has_value()) {
            return fail(body.error());
        }
        explain_out.blocks.push_back(
            ExplainBlock{"Query", std::move(*body), ExplainBlock::Format::pretty});
        sink.emit(ExecOutcome{std::move(explain_out)});
        return {};
    }

    // EXPLAIN DELETE via SCAN: the original prints the Scan block ONCE before
    // the pagination loop (delete.cpp do_scan), then runs the scan and emits a
    // DeleteItem block per returned item. Emit the single Scan block up front.
    if (explain && read.api == WriteReadApi::scan) {
        auto body = client.explain_request(read.scan);
        if (!body.has_value()) {
            return fail(body.error());
        }
        explain_out.blocks.push_back(
            ExplainBlock{"Scan", std::move(*body), ExplainBlock::Format::pretty});
    }

    std::optional<ValueMap> start_key;
    for (;;) {
        std::vector<ItemRow> items;
        std::optional<ValueMap> last_key;
        if (read.api == WriteReadApi::query) {
            QueryIn request = read.query;
            request.exclusive_start_key = start_key;
            auto result = client.query(request);
            if (!result.has_value()) {
                return fail(result.error());
            }
            items = std::move(result->items);
            last_key = std::move(result->last_evaluated_key);
        } else {
            ScanIn request = read.scan;
            request.exclusive_start_key = start_key;
            auto result = client.scan(request);
            if (!result.has_value()) {
                return fail(result.error());
            }
            items = std::move(result->items);
            last_key = std::move(result->last_evaluated_key);
        }

        for (const auto& item : items) {
            DeleteItemIn request = write_template;
            request.key = row_key(item);
            if (explain) {
                // Reached only on the scan path (query returned above).
                auto body = client.explain_request(request);
                if (!body.has_value()) {
                    return fail(body.error());
                }
                explain_out.blocks.push_back(
                    ExplainBlock{"DeleteItem", std::move(*body), ExplainBlock::Format::pretty});
                continue;
            }
            ++acct.read;
            auto result = client.delete_item(request);
            if (result.has_value()) {
                ++acct.modified;
            } else if (result.error().code == Errc::conditional_check_failed) {
                ++acct.ccf;
            } else {
                return fail(result.error());
            }
        }

        if (!last_key.has_value() || last_key->empty()) {
            break;
        }
        start_key = std::move(last_key);
    }

    if (explain) {
        sink.emit(ExecOutcome{std::move(explain_out)});
    } else {
        sink.emit(ExecOutcome{acct});
    }
    return {};
}

}  // namespace ddbsh::engine

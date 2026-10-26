// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// delete_executor.cpp — see delete_executor.hpp.

#include "execute/delete_executor.hpp"

#include <utility>

#include "execute/rate_limiter.hpp"
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
    const std::vector<std::string> base_pk_list = base->pk;
    const std::vector<std::string> base_rk_list = base->rk;
    const std::string base_pk = base_pk_list.empty() ? std::string() : base_pk_list[0];
    const std::string base_rk = base_rk_list.empty() ? std::string() : base_rk_list[0];
    const Where* where = cmd.where.get();

    // Write template FIRST (first alias prefix), per D-2c-10.
    SymbolTable write_st(next_prefix());
    DeleteItemIn write_template = make_delete_request(cmd, base_pk, base_rk, write_st);

    const bool want_rv = cmd.return_value != ReturnValue::none;

    // A WITH RATELIMIT clause meters the read (enumeration) and write buckets
    // (delete.cpp do_query/do_scan + do_delete). Never constructed when
    // explaining (no service call to meter).
    std::optional<RateLimiter> limiter;
    if (!explain && cmd.ratelimit.has_value()) {
        limiter.emplace(cmd.ratelimit->rcu, cmd.ratelimit->wcu);
    }

    // ---- single conditional delete (full base-table key) -------------------
    if (where != nullptr && where->can_delete_item(base_pk_list, base_rk_list)) {
        DeleteItemIn request = write_template;
        request.key = key_of(where->get_gud_key(base_pk_list, base_rk_list));
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
        if (limiter.has_value()) {
            limiter->writelimit();
        }
        auto result = client.delete_item(request);
        BulkWriteOutcome acct{BulkWriteOutcome::Verb::del, 0, 0, 0, std::nullopt};
        if (result.has_value()) {
            acct.modified = 1;
            if (want_rv) {
                acct.returned_items.push_back(result->attributes.value_or(ItemRow{}));
            }
            if (limiter.has_value()) {
                limiter->consume_writes(result->consumed_capacity);
            }
        } else if (result.error().code == Errc::conditional_check_failed) {
            acct.ccf = 1;
        } else {
            return fail(result.error());
        }
        sink.emit(ExecOutcome{std::move(acct)});
        return {};
    }

    // ---- bulk enumeration --------------------------------------------------
    std::optional<QueryKeyPlan> qplan;
    if (!cmd.index.has_value()) {
        qplan = where != nullptr ? where->plan_query(base_pk_list, base_rk_list) : std::nullopt;
    } else {
        auto idx = get_key_schema(client, cmd.table, *cmd.index);
        if (!idx.has_value()) {
            return fail(idx.error());
        }
        qplan = where != nullptr ? where->plan_query(idx->pk, idx->rk) : std::nullopt;
    }

    SymbolTable read_st(next_prefix());
    WriteReadPlan read = build_write_read(cmd.table, cmd.index, where, qplan, base_pk, base_rk,
                                          cmd.ratelimit.has_value(), read_st);

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
        std::optional<ConsumedCapacity> page_cc;
        if (read.api == WriteReadApi::query) {
            QueryIn request = read.query;
            request.exclusive_start_key = start_key;
            auto result = client.query(request);
            if (!result.has_value()) {
                return fail(result.error());
            }
            items = std::move(result->items);
            last_key = std::move(result->last_evaluated_key);
            page_cc = std::move(result->consumed_capacity);
        } else {
            ScanIn request = read.scan;
            request.exclusive_start_key = start_key;
            auto result = client.scan(request);
            if (!result.has_value()) {
                return fail(result.error());
            }
            items = std::move(result->items);
            last_key = std::move(result->last_evaluated_key);
            page_cc = std::move(result->consumed_capacity);
        }

        // Meter the read page (do_query/do_scan: consume_reads after a
        // successful page, before iterating its items).
        if (limiter.has_value()) {
            limiter->consume_reads(page_cc);
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
            // do_delete: writelimit() before the write, consume_writes() after.
            if (limiter.has_value()) {
                limiter->writelimit();
            }
            auto result = client.delete_item(request);
            if (result.has_value()) {
                ++acct.modified;
                if (want_rv) {
                    acct.returned_items.push_back(result->attributes.value_or(ItemRow{}));
                }
                if (limiter.has_value()) {
                    limiter->consume_writes(result->consumed_capacity);
                }
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
        // Throttle before fetching the next page (readlimit() after setting the
        // next ExclusiveStartKey).
        if (limiter.has_value()) {
            limiter->readlimit();
        }
    }

    if (explain) {
        sink.emit(ExecOutcome{std::move(explain_out)});
    } else {
        sink.emit(ExecOutcome{std::move(acct)});
    }
    return {};
}

}  // namespace ddbsh::engine

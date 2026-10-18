// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// update_executor.cpp — see update_executor.hpp.

#include "execute/update_executor.hpp"

#include <utility>

#include "execute/write_common.hpp"
#include "expressions/symbol_table.hpp"
#include "planner/get_key_schema.hpp"

namespace ddbsh::engine {

namespace {

/// Serialize one SET assignment through the write SymbolTable (port of
/// CUpdateSetElement::__serialize): `#lhs = [#b | if_not_exists(#p,:v)][op]:v`.
std::string serialize_set_element(const UpdateSetElement& elem, SymbolTable& st) {
    std::string rv = st.new_attribute(elem.lhs) + " = ";
    if (!elem.rhs_attribute.empty()) {
        rv += st.new_attribute(elem.rhs_attribute);
    } else if (!elem.if_not_exists_lhs.empty()) {
        rv += "if_not_exists(" + st.new_attribute(elem.if_not_exists_lhs) + ",";
        rv += st.new_value(*elem.if_not_exists_value);
        rv += ")";
    }
    if (!elem.op.empty()) {
        rv += elem.op;
    }
    if (elem.rhs_value.has_value()) {
        rv += st.new_value(*elem.rhs_value);
    }
    return rv;
}

/// Convert an enumerated row (base-table keys) to a sorted item key, matching
/// the SDK's std::map Key serialization order (byte-fidelity).
ValueMap row_key(const ItemRow& item) {
    KeyMap sorted;
    for (const auto& [name, value] : item.attributes) {
        sorted.emplace(name, value);
    }
    return key_of(sorted);
}

}  // namespace

UpdateItemIn make_update_request(const UpdateCmd& cmd, const std::string& pk, const std::string& rk,
                                 SymbolTable& st) {
    UpdateItemIn request;
    request.table_name = cmd.table;
    request.return_values = to_client_return_value(cmd.return_value);

    if (cmd.mutation == Mutation::set) {
        std::string expr = "SET ";
        for (std::size_t i = 0; i < cmd.set.size(); ++i) {
            if (i != 0) {
                expr += ", ";
            }
            expr += serialize_set_element(cmd.set[i], st);
        }
        request.update_expression = std::move(expr);
    } else {
        std::string expr = "REMOVE ";
        for (std::size_t i = 0; i < cmd.remove.size(); ++i) {
            if (i != 0) {
                expr += ", ";
            }
            expr += st.new_attribute(cmd.remove[i]);
        }
        request.update_expression = std::move(expr);
    }

    std::string cond;
    if (!cmd.upsert) {
        cond = "attribute_exists(" + st.new_attribute(pk) + ")";
    }
    if (cmd.where) {
        std::string qf = cmd.where->update_delete_condition_check(pk, rk, &st);
        if (!qf.empty()) {
            cond = cond.empty() ? qf : cond + " AND " + qf;
        }
    }
    if (!cond.empty()) {
        request.condition_expression = std::move(cond);
    }

    request.expression_attribute_names = names_of(st);
    request.expression_attribute_values = values_of(st);
    if (cmd.ratelimit.has_value()) {
        request.return_consumed_capacity = ddbsh::ReturnConsumedCapacity::total;
    }
    return request;
}

Result<void> execute_update(const UpdateCmd& cmd, IDDBClient& client,
                            const std::function<std::string()>& next_prefix, bool explain,
                            OutcomeSink& sink) {
    auto base = get_key_schema(client, cmd.table, "");
    if (!base.has_value()) {
        return fail(base.error());
    }
    const std::string base_pk = base->pk;
    const std::string base_rk = base->rk;
    const Where* where = cmd.where.get();

    // Write template FIRST (consumes the first alias prefix), per D-2c-10.
    SymbolTable write_st(next_prefix());
    UpdateItemIn write_template = make_update_request(cmd, base_pk, base_rk, write_st);

    const BulkWriteOutcome::Verb verb =
        cmd.upsert ? BulkWriteOutcome::Verb::upsert : BulkWriteOutcome::Verb::update;

    // ---- single conditional write (full base-table key) --------------------
    if (where != nullptr && where->can_update_item(base_pk, base_rk)) {
        UpdateItemIn request = write_template;
        request.key = key_of(where->get_gud_key(base_pk, base_rk));
        if (explain) {
            auto body = client.explain_request(request);
            if (!body.has_value()) {
                return fail(body.error());
            }
            ExplainOutcome out;
            out.blocks.push_back(
                ExplainBlock{"UpdateItem", std::move(*body), ExplainBlock::Format::pretty});
            sink.emit(ExecOutcome{std::move(out)});
            return {};
        }
        auto result = client.update_item(request);
        BulkWriteOutcome acct{verb, 0, 0, 0, std::nullopt};
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

    // ---- bulk enumeration (Query/Scan + per-item UpdateItem) ---------------
    // Resolve the key schema the WHERE is evaluated against: the index schema
    // when a `.index` is targeted, the base-table schema otherwise.
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

    // Enumeration read SymbolTable from the NEXT prefix (D-2c-10).
    SymbolTable read_st(next_prefix());
    WriteReadPlan read = build_write_read(cmd.table, cmd.index, where, is_query, key_pk, key_rk,
                                          base_pk, base_rk, cmd.ratelimit.has_value(), read_st);

    ExplainOutcome explain_out;
    BulkWriteOutcome acct{verb, 0, 0, 0, std::nullopt};

    // Paginate the enumeration; for each returned item issue (or serialize) a
    // per-item UpdateItem. EXPLAIN UPDATE runs the read on BOTH query and scan
    // paths and emits the read block per page then the per-item write blocks
    // (§8.2). Non-explain issues the writes and counts read/modified/ccf.
    std::optional<ValueMap> start_key;
    for (;;) {
        std::vector<ItemRow> items;
        std::optional<ValueMap> last_key;
        if (read.api == WriteReadApi::query) {
            QueryIn request = read.query;
            request.exclusive_start_key = start_key;
            if (explain) {
                auto body = client.explain_request(request);
                if (!body.has_value()) {
                    return fail(body.error());
                }
                explain_out.blocks.push_back(
                    ExplainBlock{"Query", std::move(*body), ExplainBlock::Format::pretty});
            }
            auto result = client.query(request);
            if (!result.has_value()) {
                return fail(result.error());
            }
            items = std::move(result->items);
            last_key = std::move(result->last_evaluated_key);
        } else {
            ScanIn request = read.scan;
            request.exclusive_start_key = start_key;
            if (explain) {
                auto body = client.explain_request(request);
                if (!body.has_value()) {
                    return fail(body.error());
                }
                explain_out.blocks.push_back(
                    ExplainBlock{"Scan", std::move(*body), ExplainBlock::Format::pretty});
            }
            auto result = client.scan(request);
            if (!result.has_value()) {
                return fail(result.error());
            }
            items = std::move(result->items);
            last_key = std::move(result->last_evaluated_key);
        }

        for (const auto& item : items) {
            UpdateItemIn request = write_template;
            request.key = row_key(item);
            if (explain) {
                auto body = client.explain_request(request);
                if (!body.has_value()) {
                    return fail(body.error());
                }
                explain_out.blocks.push_back(
                    ExplainBlock{"UpdateItem", std::move(*body), ExplainBlock::Format::pretty});
                continue;
            }
            ++acct.read;
            auto result = client.update_item(request);
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

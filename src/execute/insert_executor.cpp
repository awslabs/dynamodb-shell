// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// insert_executor.cpp — see insert_executor.hpp.

#include "execute/insert_executor.hpp"

#include <utility>

#include "execute/rate_limiter.hpp"
#include "execute/write_common.hpp"
#include "expressions/symbol_table.hpp"
#include "planner/get_key_schema.hpp"

namespace ddbsh::engine {

/// Validate that every row supplies exactly one value per named column
/// (CInsertCommand::valid). Returns a descriptive Error on the first mismatch.
Result<void> validate_insert_rows(const InsertCmd& cmd) {
    for (std::size_t row = 0; row < cmd.rows.size(); ++row) {
        if (cmd.rows[row].size() != cmd.columns.size()) {
            return fail(Errc::invalid_argument,
                        "Unequal number of column names (" + std::to_string(cmd.columns.size()) +
                            ") and values (" + std::to_string(cmd.rows[row].size()) +
                            ") in tuple " + std::to_string(row + 1));
        }
    }
    return {};
}

/// Build the PutItem request for one row (CInsertCommand::make_put_request).
/// For INSERT, aliases the partition key through a fresh SymbolTable (seeded
/// with the next alias prefix) and adds the attribute_not_exists condition.
PutItemIn make_put_request(const InsertCmd& cmd, std::size_t row, const std::string& pk,
                           const std::function<std::string()>& next_prefix) {
    PutItemIn request;
    request.table_name = cmd.table_name;
    for (std::size_t col = 0; col < cmd.columns.size(); ++col) {
        request.item.emplace_back(cmd.columns[col], cmd.rows[row][col]);
    }

    // RETURN VALUES (REPLACE may request ALL_OLD; INSERT forces none via the
    // grammar). A ratelimit forces ReturnConsumedCapacity=TOTAL so the response
    // carries CapacityUnits to meter (insert.cpp make_put_request).
    request.return_values = to_client_return_value(cmd.return_value);
    if (cmd.ratelimit.has_value()) {
        request.return_consumed_capacity = ddbsh::ReturnConsumedCapacity::total;
    }

    if (cmd.insert) {
        SymbolTable st(next_prefix());
        std::string pk_token = st.new_attribute(pk);
        request.condition_expression = "attribute_not_exists(" + pk_token + ")";
        for (const auto& [token, name] : st.get_names()) {
            request.expression_attribute_names.emplace_back(token, name);
        }
    }

    return request;
}

Result<void> execute_insert_streaming(const InsertCmd& cmd, IDDBClient& client,
                                      const std::function<std::string()>& next_prefix,
                                      OutcomeSink& sink) {
    auto schema = get_key_schema(client, cmd.table_name, "");
    if (!schema.has_value()) {
        return fail(schema.error());
    }
    const std::string& pk = schema->pk;

    if (auto valid = validate_insert_rows(cmd); !valid.has_value()) {
        return fail(valid.error());
    }

    const std::string token = cmd.insert ? "INSERT" : "REPLACE";
    const bool want_rv = cmd.return_value != ReturnValue::none;

    // A WITH RATELIMIT clause meters each write with its own token bucket
    // (insert.cpp: after a successful PutItem, consume_writes THEN writelimit —
    // note the consume-then-limit order, the REVERSE of the update/delete write
    // path; reproduce this site's own order). Output is unchanged.
    std::optional<RateLimiter> limiter;
    if (cmd.ratelimit.has_value()) {
        limiter.emplace(cmd.ratelimit->rcu, cmd.ratelimit->wcu);
    }

    for (std::size_t row = 0; row < cmd.rows.size(); ++row) {
        PutItemIn request = make_put_request(cmd, row, pk, next_prefix);
        auto result = client.put_item(request);
        if (!result.has_value()) {
            // Emit nothing for the failing row; the rows before it are already
            // emitted. Returning the Error lets the embedder render it after
            // the partial output (the original's interleave — insert.cpp).
            return fail(result.error());
        }
        // One status token per written row, emitted as it succeeds (AI-4). With
        // RETURN VALUES (REPLACE ALL_OLD), the row carries the returned item so
        // the Renderer emits `REPLACE: {item}` instead of `REPLACE` (insert.cpp
        // prints `%s: %s` when a return value was requested, even if the item
        // is empty — so request-but-empty yields a present, empty ItemRow).
        WriteOutcome out;
        out.tokens.push_back(token);
        if (want_rv) {
            out.items.push_back(result->attributes.value_or(ItemRow{}));
        }
        sink.emit(ExecOutcome{std::move(out)});

        if (limiter.has_value()) {
            limiter->consume_writes(result->consumed_capacity);
            limiter->writelimit();
        }
    }

    return {};
}

Result<ExecOutcome> execute_insert_explain(const InsertCmd& cmd, IDDBClient& client,
                                           const std::function<std::string()>& next_prefix) {
    // Resolve the partition key and validate rows exactly as execute_insert
    // (the original resolves the key schema unconditionally, even when
    // explaining), then serialize each row's PutItem instead of issuing it.
    auto schema = get_key_schema(client, cmd.table_name, "");
    if (!schema.has_value()) {
        return fail(schema.error());
    }
    const std::string& pk = schema->pk;

    if (auto valid = validate_insert_rows(cmd); !valid.has_value()) {
        return fail(valid.error());
    }

    ExplainOutcome outcome;
    outcome.blocks.reserve(cmd.rows.size());
    for (std::size_t row = 0; row < cmd.rows.size(); ++row) {
        PutItemIn request = make_put_request(cmd, row, pk, next_prefix);
        auto body = client.explain_request(request);
        if (!body.has_value()) {
            return fail(body.error());
        }
        outcome.blocks.push_back(
            ExplainBlock{"PutItem", std::move(*body), ExplainBlock::Format::pretty});
    }

    return ExecOutcome{std::move(outcome)};
}

}  // namespace ddbsh::engine

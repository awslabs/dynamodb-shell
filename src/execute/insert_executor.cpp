// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// insert_executor.cpp — see insert_executor.hpp.

#include "execute/insert_executor.hpp"

#include <utility>

#include "expressions/symbol_table.hpp"
#include "planner/get_key_schema.hpp"

namespace ddbsh::engine {

namespace {

/// Validate that every row supplies exactly one value per named column
/// (CInsertCommand::valid). Returns a descriptive Error on the first mismatch.
Result<void> validate_rows(const InsertCmd& cmd) {
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

}  // namespace

Result<ExecOutcome> execute_insert(const InsertCmd& cmd, IDDBClient& client,
                                   const std::function<std::string()>& next_prefix) {
    // Resolve the partition key (needed only for the INSERT condition, but the
    // original resolves it unconditionally — a missing table fails here too).
    auto schema = get_key_schema(client, cmd.table_name, "");
    if (!schema.has_value()) {
        return fail(schema.error());
    }
    const std::string& pk = schema->pk;

    if (auto valid = validate_rows(cmd); !valid.has_value()) {
        return fail(valid.error());
    }

    const std::string token = cmd.insert ? "INSERT" : "REPLACE";

    WriteOutcome outcome;
    outcome.tokens.reserve(cmd.rows.size());
    for (std::size_t row = 0; row < cmd.rows.size(); ++row) {
        PutItemIn request = make_put_request(cmd, row, pk, next_prefix);
        auto result = client.put_item(request);
        if (!result.has_value()) {
            // A failed row (e.g. a conditional-check failure on INSERT) stops
            // the batch, exactly as the original. This buffering form cannot
            // carry both the already-succeeded tokens and the Error, so it
            // returns the Error alone; the streaming form
            // (execute_insert_streaming) preserves the original's
            // partial-output-then-error behavior.
            return fail(result.error());
        }
        outcome.tokens.push_back(token);
    }

    return ExecOutcome{std::move(outcome)};
}

Result<void> execute_insert_streaming(const InsertCmd& cmd, IDDBClient& client,
                                      const std::function<std::string()>& next_prefix,
                                      OutcomeSink& sink) {
    auto schema = get_key_schema(client, cmd.table_name, "");
    if (!schema.has_value()) {
        return fail(schema.error());
    }
    const std::string& pk = schema->pk;

    if (auto valid = validate_rows(cmd); !valid.has_value()) {
        return fail(valid.error());
    }

    const std::string token = cmd.insert ? "INSERT" : "REPLACE";

    for (std::size_t row = 0; row < cmd.rows.size(); ++row) {
        PutItemIn request = make_put_request(cmd, row, pk, next_prefix);
        auto result = client.put_item(request);
        if (!result.has_value()) {
            // Emit nothing for the failing row; the rows before it are already
            // emitted. Returning the Error lets the embedder render it after
            // the partial output (the original's interleave — insert.cpp).
            return fail(result.error());
        }
        // One status token per written row, emitted as it succeeds (AI-4).
        sink.emit(ExecOutcome{WriteOutcome{{token}}});
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

    if (auto valid = validate_rows(cmd); !valid.has_value()) {
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

// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// transact_executor.cpp — see transact_executor.hpp.

#include "execute/transact_executor.hpp"

#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "execute/transact_build.hpp"

namespace ddbsh::engine {

namespace {

/// Classify a builder Error: a not_reducible_to_getitem code is a pre-flight
/// ABORT (record the message if any, mark aborted, keep building); any other
/// code is a hard failure the caller propagates. Returns the hard Error to
/// propagate, or nullopt when the error was absorbed as an abort.
std::optional<Error> absorb_or_propagate(const Error& error, bool& aborted,
                                         std::vector<std::string>& diagnostics) {
    if (error.code == Errc::not_reducible_to_getitem) {
        if (!error.message.empty()) {
            diagnostics.push_back(error.message);
        }
        aborted = true;
        return std::nullopt;
    }
    return error;
}

/// Serialize the one batch request for EXPLAIN, wrap it in an ExplainOutcome.
template <class RequestIn>
Result<ExecOutcome> explain_batch(IDDBClient& client, const std::string& api_name,
                                  const RequestIn& request) {
    auto body = client.explain_request(request);
    if (!body.has_value()) {
        return fail(body.error());
    }
    ExplainOutcome out;
    out.blocks.push_back(ExplainBlock{api_name, std::move(*body), ExplainBlock::Format::pretty});
    return ExecOutcome{std::move(out)};
}

Result<void> execute_transact_read(const TransactCmd& cmd, IDDBClient& client,
                                   const std::function<std::string()>& next_prefix, bool explain,
                                   OutcomeSink& sink) {
    bool aborted = cmd.rollback;
    std::vector<std::string> diagnostics;
    std::vector<TransactGet> gets;

    for (const auto& item : cmd.items) {
        const auto* select = std::get_if<SelectCmd>(&item);
        if (select == nullptr) {
            aborted = true;  // parse-error placeholder; abort without a message
            continue;
        }
        auto get = build_transact_get(*select, client, next_prefix);
        if (get.has_value()) {
            gets.push_back(std::move(*get));
            continue;
        }
        if (auto hard = absorb_or_propagate(get.error(), aborted, diagnostics); hard.has_value()) {
            return fail(*hard);
        }
    }

    if (explain) {
        if (aborted) {
            sink.emit(ExecOutcome{TransactOutcome{"ABORT", std::move(diagnostics)}});
            return {};
        }
        auto block = explain_batch(client, "TransactGetItems", TransactGetItemsIn{std::move(gets)});
        if (!block.has_value()) {
            return fail(block.error());
        }
        sink.emit(*block);
        return {};
    }

    if (aborted) {
        sink.emit(ExecOutcome{TransactOutcome{"ABORT", std::move(diagnostics)}});
        return {};  // exit 0 (D-2d-5 path 1)
    }

    auto out = client.transact_get_items(TransactGetItemsIn{std::move(gets)});
    if (!out.has_value()) {
        sink.emit(ExecOutcome{TransactOutcome{"ABORT", {}}});  // D-2d-5 path 2
        return fail(out.error());
    }

    // Render each response item; a missing/empty response renders to nothing
    // (the SelectOutcome renderer skips empty items — D-2d §7).
    SelectOutcome items;
    for (auto& response : out->responses) {
        if (response.has_value()) {
            items.items.push_back(std::move(*response));
        } else {
            items.items.push_back(ItemRow{});  // empty row -> skipped by the Renderer
        }
    }
    sink.emit(ExecOutcome{std::move(items)});
    return {};
}

Result<void> execute_transact_write(const TransactCmd& cmd, IDDBClient& client,
                                    const std::function<std::string()>& next_prefix, bool explain,
                                    OutcomeSink& sink) {
    bool aborted = cmd.rollback;
    std::vector<std::string> diagnostics;
    std::vector<TransactWriteItem> writes;

    for (const auto& item : cmd.items) {
        Result<void> step = std::visit(
            [&](const auto& node) -> Result<void> {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, InsertCmd>) {
                    auto puts = build_transact_puts(node, client, next_prefix);
                    if (!puts.has_value()) {
                        if (auto hard = absorb_or_propagate(puts.error(), aborted, diagnostics);
                            hard.has_value()) {
                            return fail(*hard);
                        }
                        return {};
                    }
                    for (auto& put : *puts) {
                        writes.emplace_back(std::move(put));
                    }
                    return {};
                } else if constexpr (std::is_same_v<T, UpdateCmd>) {
                    auto update = build_transact_update(node, client, next_prefix);
                    if (!update.has_value()) {
                        if (auto hard = absorb_or_propagate(update.error(), aborted, diagnostics);
                            hard.has_value()) {
                            return fail(*hard);
                        }
                        return {};
                    }
                    writes.emplace_back(std::move(*update));
                    return {};
                } else if constexpr (std::is_same_v<T, DeleteCmd>) {
                    auto del = build_transact_delete(node, client, next_prefix);
                    if (!del.has_value()) {
                        if (auto hard = absorb_or_propagate(del.error(), aborted, diagnostics);
                            hard.has_value()) {
                            return fail(*hard);
                        }
                        return {};
                    }
                    writes.emplace_back(std::move(*del));
                    return {};
                } else if constexpr (std::is_same_v<T, SelectCmd>) {
                    // EXISTS / NOT EXISTS SELECT -> ConditionCheck.
                    auto cc = build_transact_condition_check(node, client, next_prefix);
                    if (!cc.has_value()) {
                        if (auto hard = absorb_or_propagate(cc.error(), aborted, diagnostics);
                            hard.has_value()) {
                            return fail(*hard);
                        }
                        return {};
                    }
                    writes.emplace_back(std::move(*cc));
                    return {};
                } else {
                    // EmptyCmd / parse-error placeholder: abort without a message.
                    aborted = true;
                    return {};
                }
            },
            item);
        if (!step.has_value()) {
            return fail(step.error());
        }
    }

    if (explain) {
        if (aborted) {
            sink.emit(ExecOutcome{TransactOutcome{"ABORT", std::move(diagnostics)}});
            return {};
        }
        auto block =
            explain_batch(client, "TransactWriteItems", TransactWriteItemsIn{std::move(writes)});
        if (!block.has_value()) {
            return fail(block.error());
        }
        sink.emit(*block);
        return {};
    }

    if (aborted) {
        sink.emit(ExecOutcome{TransactOutcome{"ABORT", std::move(diagnostics)}});
        return {};  // exit 0 (D-2d-5 path 1)
    }

    auto out = client.transact_write_items(TransactWriteItemsIn{std::move(writes)});
    if (!out.has_value()) {
        sink.emit(ExecOutcome{TransactOutcome{"ABORT", {}}});  // D-2d-5 path 2
        return fail(out.error());
    }
    sink.emit(ExecOutcome{StatusOutcome{"COMMIT"}});  // D-2d-8
    return {};
}

}  // namespace

Result<void> execute_transact(const TransactCmd& cmd, IDDBClient& client,
                              const std::function<std::string()>& next_prefix, bool explain,
                              OutcomeSink& sink) {
    // Empty `begin; rollback;` renders nothing (itxt-1). (There is no empty
    // commit — the grammar makes `begin; commit;` a syntax error, D-2d-7.)
    if (cmd.items.empty()) {
        sink.emit(ExecOutcome{EmptyOutcome{}});
        return {};
    }

    if (cmd.access == TransactCmd::Access::read) {
        return execute_transact_read(cmd, client, next_prefix, explain, sink);
    }
    return execute_transact_write(cmd, client, next_prefix, explain, sink);
}

}  // namespace ddbsh::engine

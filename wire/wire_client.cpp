// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// wire_client.cpp — WireClient implementation (P6b data plane; P6c-b full
// surface; see wire_client.hpp).
//
// Each op is the same three-step pipeline: serialize the request (P6a/P6c-a),
// send it through the Transport, parse the response (P6b/P6c-b, mapping a
// `__type` document to an Error). explain_request std::visits the ExplainRequest
// variant and returns the serialized body for every alternative (SDK-free).
// ListTables pages internally (mirroring the AWS adapter's LastEvaluatedTableName
// loop); ListBackups returns one page (the adapter does too — the SHOW BACKUPS
// executor drives that loop). Five ops (the two transactions, UpdateTable,
// UpdateTimeToLive, UpdateContinuousBackups) re-wrap a service-returned Error in
// the adapter's composed failure message. The two waiters poll DescribeTable
// over the transport, classifying retry-vs-terminal by the `__type` exception
// NAME (D-P6c-7). SDK-free + terminal-free (the wire boundary rule).

#include "ddbsh/wire/wire_client.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

#include "ddbsh/wire/codec.hpp"
#include "wire/datetime.hpp"

namespace ddbsh::wire {
namespace {

// Re-wrap a service-returned Error in the adapter's composed failure message
// (§4.3). On the wire path the request-id is empty (D-P6b-2 / D-P6c-5), so the
// composed string carries an empty reqid segment (e.g. "Transaction failed.
// TransactionCanceledException. . <message>"). The code is forced to
// service_error to match the adapter (transact_failed_error and the update/
// ttl/pitr composers all return service_error regardless of the mapped type).
Error compose_wire_error(const char* prefix, const char* sep, const Error& e) {
    const std::string name = e.service_error_name.value_or("");
    const std::string reqid = e.request_id.value_or("");
    std::string composed = std::string(prefix) + name + ". " + reqid + sep + e.message;
    return Error{Errc::service_error, std::move(composed), e.service_error_name, e.request_id};
}

// Coarse retry classification for the waiters (D-P6c-7). The wire transport
// surfaces only the `__type` exception NAME, not the SDK's ShouldRetry(), so
// recognize the common transient names as "keep polling" and treat everything
// else as terminal. Acceptable because no golden exercises the wire waiter and
// the in-browser backend (P6d) settles in one or two polls.
bool is_transient_wire_error(const std::string& name) {
    return name == "ThrottlingException" || name == "ProvisionedThroughputExceededException" ||
           name == "RequestLimitExceeded" || name == "InternalServerError" ||
           name == "InternalServerErrorException" || name == "ServiceUnavailable" ||
           name == "ServiceUnavailableException";
}

// serialize -> send -> parse for an op whose response maps straight to its
// *Out (no composed error, no paging). Now routes through send_retrying so a
// retryable service error is retried (HT-1); defined as a WireClient member
// (below) so it can reach the retry seam.

}  // namespace

// ---- retry seam (HT-1) -----------------------------------------------------

Result<std::string> WireClient::send_retrying(std::string_view operation, std::string_view body) {
    // One send with bounded exponential-backoff-plus-full-jitter retry on a
    // retryable service error (§4.1). On success, a non-retryable error, or an
    // opaque transport failure the first send's bytes are returned unchanged, so
    // every currently-green path is byte-for-byte and call-count unchanged; the
    // retry arm is reachable only when a transient `__type` body comes back.
    for (unsigned attempt = 0;; ++attempt) {
        auto resp = transport_.send(operation, body);
        if (!resp) {
            // Opaque transport-level failure (no `__type` to classify): the seam
            // cannot tell "backend down" from "one blip", so do not retry
            // (D-HT1-3). Return the same Error the one-shot path returned.
            return resp;
        }
        const std::optional<std::string> name = error_type_name(*resp);
        const bool retryable = name.has_value() && is_transient_wire_error(*name);
        if (!retryable || attempt + 1 >= max_retry_attempts_) {
            // Success body, terminal error document, or attempts exhausted. On
            // exhaustion we return the last `__type` body (not a synthesized
            // error) so the caller's parse_* maps the exact byte-identical error
            // string the one-shot path would have (§4.1, fidelity).
            return resp;
        }
        retry_backoff(attempt);  // sleep, then re-send
    }
}

void WireClient::retry_backoff(unsigned attempt) {
    // Full jitter over an exponentially growing, capped ceiling (§4.5): base
    // 50ms, doubling per attempt, capped at 1000ms; actual sleep is a uniform
    // draw in [0, ceiling]. Full jitter de-synchronizes a burst (e.g. the
    // SHOW TABLES describe burst that triggers HT-1), avoiding a thundering herd.
    constexpr long long kBaseMs = 50;
    constexpr long long kCapMs = 1000;
    long long ceiling = kCapMs;
    // Guard the shift against overflow for a large attempt; by attempt 5
    // (50 * 32 = 1600ms) the ceiling already saturates at the cap anyway.
    if (attempt < 20) {
        const long long scaled = kBaseMs * (1LL << attempt);
        ceiling = std::min(scaled, kCapMs);
    }
    const auto sleep_ms = static_cast<long long>(jitter_unit() * static_cast<double>(ceiling));
    backoff_sleep(std::chrono::milliseconds{sleep_ms});
}

template <class Out>
Result<Out> WireClient::round_trip(const WireRequest& req,
                                   Result<Out> (*parse)(const std::string&)) {
    auto body = send_retrying(req.operation, req.body);
    if (!body) {
        return fail(body.error());
    }
    return parse(*body);
}

Result<void> WireClient::connect(const std::string& region, const std::string& endpoint) {
    region_ = region;
    endpoint_ = endpoint;
    // Forward the new target to the transport so a backend-bound transport (the
    // browser edition's JS SDK bridge) can re-target before the next send()
    // (D-P6f-4). The default Transport::on_connect is a no-op success, so the
    // host test doubles are unaffected. CONNECT is best-effort: the Session
    // ignores this result (the original's reconnect() was non-fatal), so a
    // refused reconnect still records the new region/endpoint locally (the
    // browser transport then fails closed, P9 D-P9-02-6).
    return transport_.on_connect(region, endpoint);
}

std::string WireClient::connection_descriptor() const {
    // The wire client is a thin shell over its Transport; the Transport knows
    // its own kind and backend binding (D-P6f-9). Report it verbatim.
    return transport_.descriptor();
}

Result<GetItemOut> WireClient::get_item(const GetItemIn& in) {
    return round_trip<GetItemOut>(serialize(in), &parse_get_item);
}

Result<QueryOut> WireClient::query(const QueryIn& in) {
    return round_trip<QueryOut>(serialize(in), &parse_query);
}

Result<ScanOut> WireClient::scan(const ScanIn& in) {
    return round_trip<ScanOut>(serialize(in), &parse_scan);
}

Result<PutItemOut> WireClient::put_item(const PutItemIn& in) {
    return round_trip<PutItemOut>(serialize(in), &parse_put_item);
}

Result<UpdateItemOut> WireClient::update_item(const UpdateItemIn& in) {
    return round_trip<UpdateItemOut>(serialize(in), &parse_update_item);
}

Result<DeleteItemOut> WireClient::delete_item(const DeleteItemIn& in) {
    return round_trip<DeleteItemOut>(serialize(in), &parse_delete_item);
}

Result<std::string> WireClient::explain_request(const ExplainRequest& req) {
    // Every ExplainRequest alternative serializes SDK-free (P6a data-plane +
    // P6c control-plane/transaction/vector); the EXPLAIN body is exactly the
    // serialized request body (P6-02 §3.2), byte-proven == the SDK's
    // SerializePayload() by the differential gate. The engine's executors wrap
    // it in ApiName(...) and apply the EXPLAIN label (including the two
    // *Request-suffixed labels), so no quirk lives here.
    return std::visit(
        [](const auto& in) -> Result<std::string> {
            using T = std::decay_t<decltype(in)>;
            if constexpr (std::is_same_v<T, RestoreToPitrIn>) {
                // The one EXPLAIN alternative that can fail to serialize: the
                // restore timestamp must parse (the SDK's WasParseSuccessful()
                // gate). Validate first and return the SDK adapter's exact
                // invalid_argument Error on failure (D-P6c-2 / §2.2), so the
                // wire path matches the SDK path byte-for-byte on both the
                // success body and the error.
                if (!iso8601_to_epoch(in.restore_datetime).has_value()) {
                    return fail(Errc::invalid_argument,
                                "Invalid restore timestamp: " + in.restore_datetime);
                }
                return serialize(in).body;
            } else {
                return serialize(in).body;
            }
        },
        req);
}

// ---- control plane / backup-restore (token-only acks + report reads) -------

Result<CreateTableOut> WireClient::create_table(const CreateTableIn& in) {
    return round_trip<CreateTableOut>(serialize(in), &parse_create_table);
}

Result<UpdateTableOut> WireClient::update_table(const UpdateTableIn& in) {
    const WireRequest req = serialize(in);
    auto body = send_retrying(req.operation, req.body);
    if (!body) {
        return fail(body.error());
    }
    auto out = parse_update_table(*body);
    if (!out) {
        return fail(compose_wire_error("Update Table failed. ", ". ", out.error()));
    }
    return out;
}

Result<UpdateTimeToLiveOut> WireClient::update_time_to_live(const UpdateTimeToLiveIn& in) {
    const WireRequest req = serialize(in);
    auto body = send_retrying(req.operation, req.body);
    if (!body) {
        return fail(body.error());
    }
    auto out = parse_update_time_to_live(*body);
    if (!out) {
        return fail(compose_wire_error("Error updating TTL. ", ".\n\t", out.error()));
    }
    return out;
}

Result<UpdateContinuousBackupsOut> WireClient::update_continuous_backups(
    const UpdateContinuousBackupsIn& in) {
    const WireRequest req = serialize(in);
    auto body = send_retrying(req.operation, req.body);
    if (!body) {
        return fail(body.error());
    }
    auto out = parse_update_continuous_backups(*body);
    if (!out) {
        return fail(compose_wire_error("Setting PITR failed. ", ".\n\t", out.error()));
    }
    return out;
}

Result<CreateBackupOut> WireClient::create_backup(const CreateBackupIn& in) {
    return round_trip<CreateBackupOut>(serialize(in), &parse_create_backup);
}

Result<DeleteBackupOut> WireClient::delete_backup(const DeleteBackupIn& in) {
    return round_trip<DeleteBackupOut>(serialize(in), &parse_delete_backup);
}

Result<DescribeBackupOut> WireClient::describe_backup(const DescribeBackupIn& in) {
    return round_trip<DescribeBackupOut>(serialize(in), &parse_describe_backup);
}

Result<RestoreFromBackupOut> WireClient::restore_table_from_backup(const RestoreFromBackupIn& in) {
    return round_trip<RestoreFromBackupOut>(serialize(in), &parse_restore_from_backup);
}

Result<RestoreToPitrOut> WireClient::restore_table_to_point_in_time(const RestoreToPitrIn& in) {
    // A malformed restore timestamp never reaches the transport — surface it as
    // the adapter's exact invalid_argument Error (D-2f-4 / §2.2), before
    // serializing (the RestoreToPitr serializer is total and assumes a valid
    // timestamp).
    if (!iso8601_to_epoch(in.restore_datetime).has_value()) {
        return fail(Errc::invalid_argument, "Invalid restore timestamp: " + in.restore_datetime);
    }
    return round_trip<RestoreToPitrOut>(serialize(in), &parse_restore_to_pitr);
}

Result<DescribeLimitsOut> WireClient::describe_limits(const DescribeLimitsIn& in) {
    return round_trip<DescribeLimitsOut>(serialize(in), &parse_describe_limits);
}

Result<ListBackupsOut> WireClient::list_backups(const ListBackupsIn& in) {
    // One page per call — the AWS adapter does the same; the SHOW BACKUPS
    // executor drives the cursor loop on last_evaluated_backup_arn.
    return round_trip<ListBackupsOut>(serialize(in), &parse_list_backups);
}

Result<DeleteTableOut> WireClient::delete_table(const DeleteTableIn& in) {
    return round_trip<DeleteTableOut>(serialize(in), &parse_delete_table);
}

Result<ListTablesOut> WireClient::list_tables(const ListTablesIn&) {
    // Page internally on LastEvaluatedTableName so the caller gets the complete
    // list, exactly as the AWS adapter's list_tables loop does, one
    // list_tables_page request at a time.
    ListTablesOut out;
    ListTablesPageIn page_in;  // no cursor => first page
    do {
        auto page = list_tables_page(page_in);
        if (!page) {
            return fail(page.error());
        }
        for (auto& name : page->table_names) {
            out.table_names.push_back(std::move(name));
        }
        page_in.exclusive_start_table_name = std::move(page->last_evaluated_table_name);
    } while (page_in.exclusive_start_table_name.has_value());
    return out;
}

Result<ListTablesPageOut> WireClient::list_tables_page(const ListTablesPageIn& in) {
    // One ListTables round trip (P12, D-P12-12); the cursor is threaded through
    // serialize_list_tables, so the wire body is the one list_tables sends. An
    // empty cursor means the first page (see ListTablesPageIn), as in
    // AwsDDBClient, so it is not sent as an ExclusiveStartTableName.
    std::optional<std::string> cursor = in.exclusive_start_table_name;
    if (cursor.has_value() && cursor->empty()) {
        cursor.reset();
    }
    const WireRequest req = serialize_list_tables(cursor);
    auto body = send_retrying(req.operation, req.body);
    if (!body) {
        return fail(body.error());
    }
    auto page = parse_list_tables(*body);
    if (!page) {
        return fail(page.error());
    }
    return ListTablesPageOut{std::move(page->table_names),
                             std::move(page->last_evaluated_table_name)};
}

Result<DescribeTableOut> WireClient::describe_table(const DescribeTableIn& in) {
    return round_trip<DescribeTableOut>(serialize(in), &parse_describe_table);
}

Result<SearchVectorsOut> WireClient::search_vectors(const SearchVectorsIn& in) {
    return round_trip<SearchVectorsOut>(serialize(in), &parse_search_vectors);
}

Result<TransactGetItemsOut> WireClient::transact_get_items(const TransactGetItemsIn& in) {
    const WireRequest req = serialize(in);
    auto body = send_retrying(req.operation, req.body);
    if (!body) {
        return fail(body.error());
    }
    auto out = parse_transact_get_items(*body);
    if (!out) {
        return fail(compose_wire_error("Transaction failed. ", ". ", out.error()));
    }
    return out;
}

Result<TransactWriteItemsOut> WireClient::transact_write_items(const TransactWriteItemsIn& in) {
    const WireRequest req = serialize(in);
    auto body = send_retrying(req.operation, req.body);
    if (!body) {
        return fail(body.error());
    }
    auto out = parse_transact_write_items(*body);
    if (!out) {
        return fail(compose_wire_error("Transaction failed. ", ". ", out.error()));
    }
    return out;
}

Result<TimeToLiveOut> WireClient::describe_time_to_live(const DescribeTimeToLiveIn& in) {
    return round_trip<TimeToLiveOut>(serialize(in), &parse_describe_time_to_live);
}

Result<ContinuousBackupsOut> WireClient::describe_continuous_backups(
    const DescribeContinuousBackupsIn& in) {
    return round_trip<ContinuousBackupsOut>(serialize(in), &parse_describe_continuous_backups);
}

Result<TableReplicaAutoScalingOut> WireClient::describe_table_replica_auto_scaling(
    const DescribeTableReplicaAutoScalingIn& in) {
    return round_trip<TableReplicaAutoScalingOut>(serialize(in),
                                                  &parse_describe_table_replica_auto_scaling);
}

void WireClient::backoff_sleep(std::chrono::milliseconds interval) {
#if defined(__EMSCRIPTEN__)
    // Asyncify yields to the browser event loop so the transport's fetch can
    // make progress between polls (P6d).
    emscripten_sleep(static_cast<unsigned int>(interval.count()));
#else
    std::this_thread::sleep_for(interval);
#endif
}

double WireClient::jitter_unit() {
    // Full-jitter fraction in [0, 1) (HT-1 §4.7). Per-instance RNG (no global
    // state): the engine may run multiple sessions concurrently.
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng_);
}

Result<void> WireClient::wait_for_table_active(const std::string& table_name) {
    // Bounded exponential backoff: 100ms doubling to a 2s cap (arch §10). Poll
    // DescribeTable until the table reports ACTIVE. CREATING/UPDATING keep
    // polling; any other status cannot progress to ACTIVE and is terminal.
    // A ResourceNotFoundException (eventual consistency right after create) or a
    // transient error keeps polling; any other service error is terminal. A
    // transport-level failure is terminal (no `__type` to classify).
    //
    // This waiter deliberately uses transport_.send directly, NOT send_retrying
    // (HT-1 §4.2): it already owns an unbounded poll-with-backoff loop that
    // treats a transient `__type` as "keep polling", so wrapping each send in a
    // second bounded retry would be redundant and could interact badly with the
    // outer loop.
    std::chrono::milliseconds interval(100);
    const std::chrono::milliseconds cap(2000);
    while (true) {
        const WireRequest req = serialize(DescribeTableIn{table_name});
        auto body = transport_.send(req.operation, req.body);
        if (!body) {
            return fail(body.error());
        }
        auto desc = parse_describe_table(*body);
        if (desc) {
            const std::string& status = desc->table_status;
            if (status == "ACTIVE") {
                return {};
            }
            if (status != "CREATING" && status != "UPDATING") {
                return fail(Error{Errc::service_error,
                                  "Table " + table_name + " is in state " + status +
                                      " and will not become ACTIVE.",
                                  std::nullopt, std::nullopt});
            }
            // CREATING/UPDATING: keep polling.
        } else {
            const std::string name = desc.error().service_error_name.value_or("");
            if (name != "ResourceNotFoundException" && !is_transient_wire_error(name)) {
                return fail(desc.error());
            }
            // not-found (eventual consistency) or transient: keep polling.
        }
        backoff_sleep(interval);
        interval = std::min(interval * 2, cap);
    }
}

Result<void> WireClient::wait_for_table_gone(const std::string& table_name) {
    // Bounded exponential backoff: 100ms doubling to a 2s cap (arch §10). Poll
    // DescribeTable until it reports ResourceNotFoundException (gone). A table
    // that still exists keeps polling; a transient error keeps polling; any
    // other service error is terminal; a transport-level failure is terminal.
    //
    // Like wait_for_table_active, this waiter uses transport_.send directly, NOT
    // send_retrying (HT-1 §4.2): its own poll loop already absorbs transient
    // errors by retrying.
    std::chrono::milliseconds interval(100);
    const std::chrono::milliseconds cap(2000);
    while (true) {
        const WireRequest req = serialize(DescribeTableIn{table_name});
        auto body = transport_.send(req.operation, req.body);
        if (!body) {
            return fail(body.error());
        }
        auto desc = parse_describe_table(*body);
        if (!desc) {
            const std::string name = desc.error().service_error_name.value_or("");
            if (name == "ResourceNotFoundException") {
                return {};  // gone
            }
            if (!is_transient_wire_error(name)) {
                return fail(desc.error());
            }
            // transient: keep polling.
        }
        // else: table still exists -> keep polling.
        backoff_sleep(interval);
        interval = std::min(interval * 2, cap);
    }
}

}  // namespace ddbsh::wire

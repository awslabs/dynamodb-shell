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
#include <string>
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
// *Out (no composed error, no paging).
template <class Out>
Result<Out> round_trip(Transport& transport, const WireRequest& req,
                       Result<Out> (*parse)(const std::string&)) {
    auto body = transport.send(req.operation, req.body);
    if (!body) {
        return fail(body.error());
    }
    return parse(*body);
}

}  // namespace

Result<void> WireClient::connect(const std::string& region, const std::string& endpoint) {
    region_ = region;
    endpoint_ = endpoint;
    return {};
}

std::string WireClient::connection_descriptor() const {
    // The wire client is a thin shell over its Transport; the Transport knows
    // its own kind and backend binding (D-P6f-9). Report it verbatim.
    return transport_.descriptor();
}

Result<GetItemOut> WireClient::get_item(const GetItemIn& in) {
    return round_trip<GetItemOut>(transport_, serialize(in), &parse_get_item);
}

Result<QueryOut> WireClient::query(const QueryIn& in) {
    return round_trip<QueryOut>(transport_, serialize(in), &parse_query);
}

Result<ScanOut> WireClient::scan(const ScanIn& in) {
    return round_trip<ScanOut>(transport_, serialize(in), &parse_scan);
}

Result<PutItemOut> WireClient::put_item(const PutItemIn& in) {
    return round_trip<PutItemOut>(transport_, serialize(in), &parse_put_item);
}

Result<UpdateItemOut> WireClient::update_item(const UpdateItemIn& in) {
    return round_trip<UpdateItemOut>(transport_, serialize(in), &parse_update_item);
}

Result<DeleteItemOut> WireClient::delete_item(const DeleteItemIn& in) {
    return round_trip<DeleteItemOut>(transport_, serialize(in), &parse_delete_item);
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
    return round_trip<CreateTableOut>(transport_, serialize(in), &parse_create_table);
}

Result<UpdateTableOut> WireClient::update_table(const UpdateTableIn& in) {
    const WireRequest req = serialize(in);
    auto body = transport_.send(req.operation, req.body);
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
    auto body = transport_.send(req.operation, req.body);
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
    auto body = transport_.send(req.operation, req.body);
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
    return round_trip<CreateBackupOut>(transport_, serialize(in), &parse_create_backup);
}

Result<DeleteBackupOut> WireClient::delete_backup(const DeleteBackupIn& in) {
    return round_trip<DeleteBackupOut>(transport_, serialize(in), &parse_delete_backup);
}

Result<DescribeBackupOut> WireClient::describe_backup(const DescribeBackupIn& in) {
    return round_trip<DescribeBackupOut>(transport_, serialize(in), &parse_describe_backup);
}

Result<RestoreFromBackupOut> WireClient::restore_table_from_backup(const RestoreFromBackupIn& in) {
    return round_trip<RestoreFromBackupOut>(transport_, serialize(in), &parse_restore_from_backup);
}

Result<RestoreToPitrOut> WireClient::restore_table_to_point_in_time(const RestoreToPitrIn& in) {
    // A malformed restore timestamp never reaches the transport — surface it as
    // the adapter's exact invalid_argument Error (D-2f-4 / §2.2), before
    // serializing (the RestoreToPitr serializer is total and assumes a valid
    // timestamp).
    if (!iso8601_to_epoch(in.restore_datetime).has_value()) {
        return fail(Errc::invalid_argument, "Invalid restore timestamp: " + in.restore_datetime);
    }
    return round_trip<RestoreToPitrOut>(transport_, serialize(in), &parse_restore_to_pitr);
}

Result<DescribeLimitsOut> WireClient::describe_limits(const DescribeLimitsIn& in) {
    return round_trip<DescribeLimitsOut>(transport_, serialize(in), &parse_describe_limits);
}

Result<ListBackupsOut> WireClient::list_backups(const ListBackupsIn& in) {
    // One page per call — the AWS adapter does the same; the SHOW BACKUPS
    // executor drives the cursor loop on last_evaluated_backup_arn.
    return round_trip<ListBackupsOut>(transport_, serialize(in), &parse_list_backups);
}

Result<DeleteTableOut> WireClient::delete_table(const DeleteTableIn& in) {
    return round_trip<DeleteTableOut>(transport_, serialize(in), &parse_delete_table);
}

Result<ListTablesOut> WireClient::list_tables(const ListTablesIn&) {
    // Page internally on LastEvaluatedTableName so the caller gets the complete
    // list, exactly as the AWS adapter's list_tables loop (ListTablesIn carries
    // no cursor field, so the cursor is threaded through serialize_list_tables).
    ListTablesOut out;
    std::optional<std::string> cursor;  // nullopt => first page
    do {
        const WireRequest req = serialize_list_tables(cursor);
        auto body = transport_.send(req.operation, req.body);
        if (!body) {
            return fail(body.error());
        }
        auto page = parse_list_tables(*body);
        if (!page) {
            return fail(page.error());
        }
        for (auto& name : page->table_names) {
            out.table_names.push_back(std::move(name));
        }
        cursor = page->last_evaluated_table_name;
    } while (cursor.has_value());
    return out;
}

Result<DescribeTableOut> WireClient::describe_table(const DescribeTableIn& in) {
    return round_trip<DescribeTableOut>(transport_, serialize(in), &parse_describe_table);
}

Result<SearchVectorsOut> WireClient::search_vectors(const SearchVectorsIn& in) {
    return round_trip<SearchVectorsOut>(transport_, serialize(in), &parse_search_vectors);
}

Result<TransactGetItemsOut> WireClient::transact_get_items(const TransactGetItemsIn& in) {
    const WireRequest req = serialize(in);
    auto body = transport_.send(req.operation, req.body);
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
    auto body = transport_.send(req.operation, req.body);
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
    return round_trip<TimeToLiveOut>(transport_, serialize(in), &parse_describe_time_to_live);
}

Result<ContinuousBackupsOut> WireClient::describe_continuous_backups(
    const DescribeContinuousBackupsIn& in) {
    return round_trip<ContinuousBackupsOut>(transport_, serialize(in),
                                            &parse_describe_continuous_backups);
}

Result<TableReplicaAutoScalingOut> WireClient::describe_table_replica_auto_scaling(
    const DescribeTableReplicaAutoScalingIn& in) {
    return round_trip<TableReplicaAutoScalingOut>(transport_, serialize(in),
                                                  &parse_describe_table_replica_auto_scaling);
}

void WireClient::poll_delay(std::chrono::milliseconds interval) {
#if defined(__EMSCRIPTEN__)
    // Asyncify yields to the browser event loop so the transport's fetch/
    // ExtendDB call can make progress between polls (P6d).
    emscripten_sleep(static_cast<unsigned int>(interval.count()));
#else
    std::this_thread::sleep_for(interval);
#endif
}

Result<void> WireClient::wait_for_table_active(const std::string& table_name) {
    // Bounded exponential backoff: 100ms doubling to a 2s cap (arch §10). Poll
    // DescribeTable until the table reports ACTIVE. CREATING/UPDATING keep
    // polling; any other status cannot progress to ACTIVE and is terminal.
    // A ResourceNotFoundException (eventual consistency right after create) or a
    // transient error keeps polling; any other service error is terminal. A
    // transport-level failure is terminal (no `__type` to classify).
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
        poll_delay(interval);
        interval = std::min(interval * 2, cap);
    }
}

Result<void> WireClient::wait_for_table_gone(const std::string& table_name) {
    // Bounded exponential backoff: 100ms doubling to a 2s cap (arch §10). Poll
    // DescribeTable until it reports ResourceNotFoundException (gone). A table
    // that still exists keeps polling; a transient error keeps polling; any
    // other service error is terminal; a transport-level failure is terminal.
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
        poll_delay(interval);
        interval = std::min(interval * 2, cap);
    }
}

}  // namespace ddbsh::wire

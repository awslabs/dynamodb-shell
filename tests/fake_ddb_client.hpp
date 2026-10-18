// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// fake_ddb_client.hpp — an in-process IDDBClient test double (T7).
//
// Proves the engine is unit-testable with NO AWS account: it fills the
// library's SDK-free structs directly. Table key schemas are registered up
// front via set_table() so the planner can resolve pk/rk offline (W4); canned
// read responses are registered via set_items()/set_pages() and consumed
// capacity via set_consumed_capacity() so SELECT execution (W5) — including
// Query/Scan pagination and the consumed-capacity totalizer — is exercised
// fully offline. Registration happens before any Session runs, and the read
// ops only read the registry, so sharing one instance across the
// multithreaded reentrancy test's sessions remains safe (those sessions never
// touch the registry).

#ifndef DDBSH_TESTS_FAKE_DDB_CLIENT_HPP
#define DDBSH_TESTS_FAKE_DDB_CLIENT_HPP

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ddbsh/iddbclient.hpp"

namespace ddbsh::test {

/// A minimal fake: describe_table serves schemas registered with set_table();
/// get_item/query/scan serve canned items (with optional multi-page cursors)
/// and consumed capacity. Extended per sub-phase as execution ports need.
class FakeDDBClient : public ddbsh::IDDBClient {
public:
    /// Register a table's key schema (and optional secondary indexes) so the
    /// planner can resolve pk/rk without a live backend.
    void set_table(DescribeTableOut schema) {
        tables_.insert_or_assign(schema.table_name, std::move(schema));
    }

    /// The table names list_tables() returns, in order (SHOW TABLES input).
    void set_table_names(std::vector<std::string> names) { table_names_ = std::move(names); }

    /// Make list_tables() fail with this Error. nullopt => succeed.
    void set_list_tables_error(std::optional<Error> error) { list_error_ = std::move(error); }

    /// Register a table's TimeToLive description (SHOW TABLES / DESCRIBE).
    void set_ttl(const std::string& table, TimeToLiveOut ttl) {
        ttls_.insert_or_assign(table, std::move(ttl));
    }

    /// Make describe_time_to_live() fail for `table` (the has_ttl()==false path
    /// that collapses a SHOW TABLES row to just the name).
    void set_ttl_error(const std::string& table, Error error) {
        ttl_errors_.insert_or_assign(table, std::move(error));
    }

    /// Register a table's continuous-backups (PITR) description (DESCRIBE).
    void set_continuous_backups(const std::string& table, ContinuousBackupsOut cb) {
        continuous_backups_.insert_or_assign(table, std::move(cb));
    }

    /// Make describe_continuous_backups() fail for `table` (the DESCRIBE PITR
    /// section is then skipped — the original's IsSuccess() guard).
    void set_continuous_backups_error(const std::string& table, Error error) {
        continuous_backups_errors_.insert_or_assign(table, std::move(error));
    }

    /// Register a table's replica auto-scaling description (DESCRIBE).
    void set_auto_scaling(const std::string& table, TableReplicaAutoScalingOut as) {
        auto_scaling_.insert_or_assign(table, std::move(as));
    }

    /// Make describe_table_replica_auto_scaling() fail for `table` (the DESCRIBE
    /// replica section is then skipped — the original's IsSuccess() guard).
    void set_auto_scaling_error(const std::string& table, Error error) {
        auto_scaling_errors_.insert_or_assign(table, std::move(error));
    }

    /// Canned item for the next get_item (nullopt => item-not-found).
    void set_get_item(std::optional<ItemRow> item) { get_item_ = std::move(item); }

    /// Canned single-page items served by the next query/scan.
    void set_items(std::vector<ItemRow> items) {
        pages_.clear();
        pages_.push_back(std::move(items));
    }

    /// Canned multi-page items: each page is returned in order, with a
    /// synthetic LastEvaluatedKey set on every page but the last so the
    /// executor exercises its pagination loop.
    void set_pages(std::vector<std::vector<ItemRow>> pages) { pages_ = std::move(pages); }

    /// Consumed capacity attached to every read response (per page).
    void set_consumed_capacity(ConsumedCapacity cc) { consumed_capacity_ = std::move(cc); }

    /// Record the number of query/scan calls so pagination can be asserted.
    int read_calls() const { return read_calls_; }

    /// Make the next put_item(s) fail with this Error (e.g. a conditional-check
    /// failure for INSERT-on-existing). nullopt => succeed.
    void set_put_error(std::optional<Error> error) { put_error_ = std::move(error); }

    /// Make ONLY the put_item call at 0-based `index` fail with `error` (the
    /// others succeed) — drives the streaming multi-row partial-output path.
    void set_put_error_at(std::size_t index, Error error) {
        put_error_ = std::move(error);
        put_error_at_ = index;
    }

    /// Make the next update_item(s) fail with this Error (e.g. a
    /// conditional-check failure). nullopt => succeed.
    void set_update_error(std::optional<Error> error) { update_error_ = std::move(error); }

    /// Make the next delete_item(s) fail with this Error. nullopt => succeed.
    void set_delete_item_error(std::optional<Error> error) {
        delete_item_error_ = std::move(error);
    }

    /// The UpdateItem requests issued, in order (for request-shape assertions).
    const std::vector<UpdateItemIn>& update_requests() const { return update_requests_; }

    /// The DeleteItem requests issued, in order (for request-shape assertions).
    const std::vector<DeleteItemIn>& delete_item_requests() const { return delete_item_requests_; }

    /// Make delete_table fail with this Error. nullopt => succeed.
    void set_delete_error(std::optional<Error> error) { delete_error_ = std::move(error); }

    /// Make create_table fail with this Error (e.g. a ResourceInUse "Table
    /// already exists" on a plain CREATE). nullopt => succeed.
    void set_create_error(std::optional<Error> error) { create_error_ = std::move(error); }

    /// The CreateTable requests issued, in order (for request-shape assertions).
    const std::vector<CreateTableIn>& create_requests() const { return create_requests_; }

    /// Table names passed to wait_for_table_active, in order.
    const std::vector<std::string>& waited_active() const { return waited_active_; }

    /// The PutItem requests issued, in order (for request-shape assertions).
    const std::vector<PutItemIn>& put_requests() const { return put_requests_; }

    /// Table names passed to delete_table, in order.
    const std::vector<std::string>& deleted_tables() const { return deleted_tables_; }

    /// Table names passed to wait_for_table_gone, in order.
    const std::vector<std::string>& waited_gone() const { return waited_gone_; }

    Result<GetItemOut> get_item(const GetItemIn&) override {
        GetItemOut out;
        out.item = get_item_;
        out.consumed_capacity = consumed_capacity_;
        return out;
    }

    Result<QueryOut> query(const QueryIn& in) override {
        ++read_calls_;
        QueryOut out;
        std::size_t page = page_index(in.exclusive_start_key);
        if (page < pages_.size()) {
            out.items = pages_[page];
        }
        out.last_evaluated_key = next_cursor(page);
        out.consumed_capacity = consumed_capacity_;
        return out;
    }

    Result<ScanOut> scan(const ScanIn& in) override {
        ++read_calls_;
        ScanOut out;
        std::size_t page = page_index(in.exclusive_start_key);
        if (page < pages_.size()) {
            out.items = pages_[page];
        }
        out.last_evaluated_key = next_cursor(page);
        out.consumed_capacity = consumed_capacity_;
        return out;
    }

    Result<PutItemOut> put_item(const PutItemIn& in) override {
        const std::size_t call = put_requests_.size();
        put_requests_.push_back(in);
        if (put_error_.has_value() && (!put_error_at_.has_value() || call == *put_error_at_)) {
            return fail(*put_error_);
        }
        return PutItemOut{};
    }

    Result<UpdateItemOut> update_item(const UpdateItemIn& in) override {
        update_requests_.push_back(in);
        if (update_error_.has_value()) {
            return fail(*update_error_);
        }
        return UpdateItemOut{};
    }

    Result<DeleteItemOut> delete_item(const DeleteItemIn& in) override {
        delete_item_requests_.push_back(in);
        if (delete_item_error_.has_value()) {
            return fail(*delete_item_error_);
        }
        return DeleteItemOut{};
    }

    Result<TransactGetItemsOut> transact_get_items(const TransactGetItemsIn& in) override {
        transact_get_requests_.push_back(in);
        if (transact_get_error_.has_value()) {
            return fail(*transact_get_error_);
        }
        TransactGetItemsOut out;
        out.responses = transact_get_responses_;
        return out;
    }

    Result<TransactWriteItemsOut> transact_write_items(const TransactWriteItemsIn& in) override {
        transact_write_requests_.push_back(in);
        if (transact_write_error_.has_value()) {
            return fail(*transact_write_error_);
        }
        return TransactWriteItemsOut{};
    }

    /// Canned responses for the next transact_get_items (one per Get).
    void set_transact_get_responses(std::vector<std::optional<ItemRow>> responses) {
        transact_get_responses_ = std::move(responses);
    }

    /// Make transact_get_items fail with this Error. nullopt => succeed.
    void set_transact_get_error(std::optional<Error> error) {
        transact_get_error_ = std::move(error);
    }

    /// Make transact_write_items fail with this Error (e.g. a cancellation).
    void set_transact_write_error(std::optional<Error> error) {
        transact_write_error_ = std::move(error);
    }

    /// The TransactGetItems requests issued, in order (for shape assertions).
    const std::vector<TransactGetItemsIn>& transact_get_requests() const {
        return transact_get_requests_;
    }

    /// The TransactWriteItems requests issued, in order (for shape assertions).
    const std::vector<TransactWriteItemsIn>& transact_write_requests() const {
        return transact_write_requests_;
    }

    Result<CreateTableOut> create_table(const CreateTableIn& in) override {
        create_requests_.push_back(in);
        if (create_error_.has_value()) {
            return fail(*create_error_);
        }
        return CreateTableOut{};
    }

    Result<void> wait_for_table_active(const std::string& table_name) override {
        waited_active_.push_back(table_name);
        return {};
    }

    Result<DeleteTableOut> delete_table(const DeleteTableIn& in) override {
        deleted_tables_.push_back(in.table_name);
        if (delete_error_.has_value()) {
            return fail(*delete_error_);
        }
        return DeleteTableOut{};
    }

    Result<void> wait_for_table_gone(const std::string& table_name) override {
        waited_gone_.push_back(table_name);
        return {};
    }

    Result<ListTablesOut> list_tables(const ListTablesIn&) override {
        if (list_error_.has_value()) {
            return fail(*list_error_);
        }
        ListTablesOut out;
        out.table_names = table_names_;
        return out;
    }

    Result<DescribeTableOut> describe_table(const DescribeTableIn& in) override {
        auto it = tables_.find(in.table_name);
        if (it == tables_.end()) {
            return fail(Errc::no_such_table, "Table: " + in.table_name + " not found");
        }
        return it->second;
    }

    Result<TimeToLiveOut> describe_time_to_live(const DescribeTimeToLiveIn& in) override {
        auto err = ttl_errors_.find(in.table_name);
        if (err != ttl_errors_.end()) {
            return fail(err->second);
        }
        auto it = ttls_.find(in.table_name);
        if (it != ttls_.end()) {
            return it->second;
        }
        // Default: TTL disabled (the common real-world case).
        return TimeToLiveOut{"DISABLED", ""};
    }

    Result<ContinuousBackupsOut> describe_continuous_backups(
        const DescribeContinuousBackupsIn& in) override {
        auto err = continuous_backups_errors_.find(in.table_name);
        if (err != continuous_backups_errors_.end()) {
            return fail(err->second);
        }
        auto it = continuous_backups_.find(in.table_name);
        if (it != continuous_backups_.end()) {
            return it->second;
        }
        // Default: PITR disabled.
        return ContinuousBackupsOut{"DISABLED", "", ""};
    }

    Result<TableReplicaAutoScalingOut> describe_table_replica_auto_scaling(
        const DescribeTableReplicaAutoScalingIn& in) override {
        auto err = auto_scaling_errors_.find(in.table_name);
        if (err != auto_scaling_errors_.end()) {
            return fail(err->second);
        }
        auto it = auto_scaling_.find(in.table_name);
        if (it != auto_scaling_.end()) {
            return it->second;
        }
        // Default: no replicas.
        return TableReplicaAutoScalingOut{};
    }

    // The fake cannot reproduce the AWS SDK's byte-exact SerializePayload()
    // (that IS the point of the real adapter — golden parity validates it via
    // the real AwsDDBClient, W7). For offline plumbing tests it records the
    // request and returns a deterministic placeholder body WITH a tab, so an
    // executor-path test still exercises the Renderer's explain_string
    // transform; byte-exactness of the JSON is a golden concern, not a fake one.
    Result<std::string> explain_request(const ExplainRequest& request) override {
        explain_requests_.push_back(request);
        if (explain_error_.has_value()) {
            return fail(*explain_error_);
        }
        return explain_body_;
    }

    /// Set the placeholder body explain_request returns (default has a tab so
    /// the Renderer transform is exercised).
    void set_explain_body(std::string body) { explain_body_ = std::move(body); }

    /// Make explain_request fail with this Error. nullopt => succeed.
    void set_explain_error(std::optional<Error> error) { explain_error_ = std::move(error); }

    /// The requests passed to explain_request, in order.
    const std::vector<ExplainRequest>& explain_requests() const { return explain_requests_; }

private:
    // The synthetic pagination cursor encodes the NEXT page index as a single
    // attribute `__page` (number). page 0 has no cursor on entry.
    std::size_t page_index(const std::optional<ValueMap>& start_key) const {
        if (!start_key.has_value()) {
            return 0;
        }
        for (const auto& [name, value] : *start_key) {
            if (name == "__page") {
                return static_cast<std::size_t>(std::stoul(value.s));
            }
        }
        return 0;
    }

    std::optional<ValueMap> next_cursor(std::size_t page) const {
        if (page + 1 >= pages_.size()) {
            return std::nullopt;  // last (or only) page
        }
        ValueMap key;
        key.emplace_back("__page", Value::number_value(std::to_string(page + 1)));
        return key;
    }

    std::map<std::string, DescribeTableOut> tables_;
    std::vector<std::string> table_names_;
    std::optional<Error> list_error_;
    std::map<std::string, TimeToLiveOut> ttls_;
    std::map<std::string, Error> ttl_errors_;
    std::map<std::string, ContinuousBackupsOut> continuous_backups_;
    std::map<std::string, Error> continuous_backups_errors_;
    std::map<std::string, TableReplicaAutoScalingOut> auto_scaling_;
    std::map<std::string, Error> auto_scaling_errors_;
    std::optional<ItemRow> get_item_;
    std::vector<std::vector<ItemRow>> pages_;
    std::optional<ConsumedCapacity> consumed_capacity_;
    int read_calls_ = 0;
    std::vector<PutItemIn> put_requests_;
    std::optional<Error> put_error_;
    std::optional<std::size_t> put_error_at_;
    std::vector<UpdateItemIn> update_requests_;
    std::optional<Error> update_error_;
    std::vector<DeleteItemIn> delete_item_requests_;
    std::optional<Error> delete_item_error_;
    std::vector<TransactGetItemsIn> transact_get_requests_;
    std::vector<std::optional<ItemRow>> transact_get_responses_;
    std::optional<Error> transact_get_error_;
    std::vector<TransactWriteItemsIn> transact_write_requests_;
    std::optional<Error> transact_write_error_;
    std::optional<Error> delete_error_;
    std::vector<std::string> deleted_tables_;
    std::vector<std::string> waited_gone_;
    std::vector<CreateTableIn> create_requests_;
    std::optional<Error> create_error_;
    std::vector<std::string> waited_active_;
    std::vector<ExplainRequest> explain_requests_;
    std::string explain_body_ = "{\n\t\"stub\":\t\"explain\"\n}";
    std::optional<Error> explain_error_;
};

}  // namespace ddbsh::test

#endif  // DDBSH_TESTS_FAKE_DDB_CLIENT_HPP

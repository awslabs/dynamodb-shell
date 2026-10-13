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
        put_requests_.push_back(in);
        if (put_error_.has_value()) {
            return fail(*put_error_);
        }
        return PutItemOut{};
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

    Result<ListTablesOut> list_tables(const ListTablesIn&) override { return ListTablesOut{}; }

    Result<DescribeTableOut> describe_table(const DescribeTableIn& in) override {
        auto it = tables_.find(in.table_name);
        if (it == tables_.end()) {
            return fail(Errc::no_such_table, "Table: " + in.table_name + " not found");
        }
        return it->second;
    }

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
    std::optional<ItemRow> get_item_;
    std::vector<std::vector<ItemRow>> pages_;
    std::optional<ConsumedCapacity> consumed_capacity_;
    int read_calls_ = 0;
    std::vector<PutItemIn> put_requests_;
    std::optional<Error> put_error_;
    std::optional<Error> delete_error_;
    std::vector<std::string> deleted_tables_;
    std::vector<std::string> waited_gone_;
    std::vector<CreateTableIn> create_requests_;
    std::optional<Error> create_error_;
    std::vector<std::string> waited_active_;
};

}  // namespace ddbsh::test

#endif  // DDBSH_TESTS_FAKE_DDB_CLIENT_HPP

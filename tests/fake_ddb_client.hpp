// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// fake_ddb_client.hpp — an in-process IDDBClient test double (T7).
//
// Proves the engine is unit-testable with NO AWS account: it fills the
// library's SDK-free structs directly. Table key schemas are registered up
// front via set_table() so the planner can resolve pk/rk offline (W4); the
// data-plane ops still return empty successful responses until execution ports
// (W5) extend this with canned items. Registration happens before any Session
// runs, and describe_table only reads the registry, so sharing one instance
// across the multithreaded reentrancy test's sessions remains safe (the
// walking-path sessions never touch the registry).

#ifndef DDBSH_TESTS_FAKE_DDB_CLIENT_HPP
#define DDBSH_TESTS_FAKE_DDB_CLIENT_HPP

#include <map>
#include <string>
#include <utility>

#include "ddbsh/iddbclient.hpp"

namespace ddbsh::test {

/// A minimal fake: data-plane ops return empty successful responses;
/// describe_table serves schemas registered with set_table(). Extended per
/// sub-phase with canned items as execution ports need them.
class FakeDDBClient : public ddbsh::IDDBClient {
public:
    /// Register a table's key schema (and optional secondary indexes) so the
    /// planner can resolve pk/rk without a live backend.
    void set_table(DescribeTableOut schema) {
        tables_.insert_or_assign(schema.table_name, std::move(schema));
    }

    Result<GetItemOut> get_item(const GetItemIn&) override { return GetItemOut{}; }
    Result<QueryOut> query(const QueryIn&) override { return QueryOut{}; }
    Result<ScanOut> scan(const ScanIn&) override { return ScanOut{}; }
    Result<PutItemOut> put_item(const PutItemIn&) override { return PutItemOut{}; }
    Result<ListTablesOut> list_tables(const ListTablesIn&) override { return ListTablesOut{}; }

    Result<DescribeTableOut> describe_table(const DescribeTableIn& in) override {
        auto it = tables_.find(in.table_name);
        if (it == tables_.end()) {
            return fail(Errc::no_such_table, "Table: " + in.table_name + " not found");
        }
        return it->second;
    }

private:
    std::map<std::string, DescribeTableOut> tables_;
};

}  // namespace ddbsh::test

#endif  // DDBSH_TESTS_FAKE_DDB_CLIENT_HPP

// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// fake_ddb_client.hpp — an in-process IDDBClient test double (T7).
//
// Proves the engine is unit-testable with NO AWS account: it fills the
// library's SDK-free structs directly. Thread-safe by construction (it holds
// no mutable state in 2a), so it can be shared across the multithreaded
// reentrancy test's sessions (P2a-02 §7.3).

#ifndef DDBSH_TESTS_FAKE_DDB_CLIENT_HPP
#define DDBSH_TESTS_FAKE_DDB_CLIENT_HPP

#include "ddbsh/iddbclient.hpp"

namespace ddbsh::test {

/// A minimal fake: returns empty successful responses. Extended per sub-phase
/// with canned tables/items as execution ports need them.
class FakeDDBClient : public ddbsh::IDDBClient {
public:
    Result<GetItemOut> get_item(const GetItemIn&) override { return GetItemOut{}; }
    Result<QueryOut> query(const QueryIn&) override { return QueryOut{}; }
    Result<ScanOut> scan(const ScanIn&) override { return ScanOut{}; }
    Result<PutItemOut> put_item(const PutItemIn&) override { return PutItemOut{}; }
    Result<ListTablesOut> list_tables(const ListTablesIn&) override { return ListTablesOut{}; }
};

}  // namespace ddbsh::test

#endif  // DDBSH_TESTS_FAKE_DDB_CLIENT_HPP

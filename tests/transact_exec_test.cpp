// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// transact_exec_test.cpp — 2d transaction tests (parse + execute, offline).
//
// Exercises the transaction family through the public Session API + the
// FakeDDBClient:
//   * the parse crux (D-2d-1): one parse_next() consumes a whole
//     begin;...;(commit|rollback) and yields ONE TransactCmd; `begin; commit;`
//     and a mixed read/write body are syntax errors; `begin; rollback;` is the
//     empty transaction.
//   * read transactions: all-GetItem-reducible SELECTs -> TransactGetItems ->
//     items only (no token); a non-reducible SELECT -> pre-flight ABORT + the
//     byte-exact stderr diagnostic (exit 0); ROLLBACK -> ABORT.
//   * write transactions: INSERT/UPDATE/DELETE/EXISTS -> TransactWriteItems ->
//     COMMIT; a service cancellation -> ABORT (emitted) THEN the composed
//     "Transaction failed. ..." Error (exit nonzero); a non-full-key UPDATE ->
//     pre-flight ABORT (no diagnostic).
//   * EXPLAIN: committed -> one batch block; aborted/rolled-back -> ABORT.

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/outcome_sink.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"
#include "ddbsh/statement.hpp"

#include "fake_ddb_client.hpp"
#include "parser/ast.hpp"

namespace {

using ddbsh::Errc;
using ddbsh::Error;
using ddbsh::ExecOutcome;
using ddbsh::ItemRow;
using ddbsh::StatementKind;
using ddbsh::TableKeySchema;
using ddbsh::Value;
using ddbsh::engine::TransactCmd;

ddbsh::DescribeTableOut table_pk(const std::string& name, const std::string& pk,
                                 const std::string& rk = "") {
    ddbsh::DescribeTableOut t;
    t.table_name = name;
    t.key_schema = ddbsh::test::make_key_schema(pk, rk);
    return t;
}

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

std::string render(const ExecOutcome& outcome) {
    return ddbsh::render::Renderer{}.render(outcome);
}

std::string render_stderr(const ExecOutcome& outcome) {
    return ddbsh::render::Renderer{}.render_stderr(outcome);
}

ItemRow num_row(std::vector<std::pair<std::string, std::string>> fields) {
    ItemRow row;
    for (auto& [name, n] : fields) {
        row.attributes.emplace_back(name, Value::number_value(n));
    }
    return row;
}

// Register the tables used across these tests.
void register_tables(ddbsh::test::FakeDDBClient& client) {
    client.set_table(table_pk("itxt1", "a"));          // hash only
    client.set_table(table_pk("itxt2", "a", "b"));     // hash + range
    client.set_table(table_pk("balances", "id"));      // hash only
    client.set_table(table_pk("transactions", "id"));  // hash only
}

struct RecordingSink : ddbsh::OutcomeSink {
    std::vector<ExecOutcome> fragments;
    void emit(const ExecOutcome& fragment) override { fragments.push_back(fragment); }
};

// ---- Parse crux (D-2d-1) ---------------------------------------------------

TEST(TransactParse, OneParseConsumesWholeTransaction) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto result = session.parse("begin; select * from itxt1 where a = 1; commit;");
    ASSERT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error().message);
    EXPECT_EQ(result->kind(), StatementKind::transaction);
    const auto& txn = std::get<TransactCmd>(result->ast()->command);
    EXPECT_EQ(txn.access, TransactCmd::Access::read);
    EXPECT_FALSE(txn.rollback);
    EXPECT_EQ(txn.items.size(), 1u);
}

TEST(TransactParse, BatchHasExactlyOneStatement) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    // Three semicolons inside, but ONE statement out (the crux).
    auto batch = session.parse_batch(
        "begin; select * from itxt1 where a = 1; select * from itxt1 where a = 2; commit;");
    EXPECT_FALSE(batch.error.has_value());
    ASSERT_EQ(batch.statements.size(), 1u);
    EXPECT_EQ(batch.statements[0].kind(), StatementKind::transaction);
}

TEST(TransactParse, EmptyRollbackIsEmptyTransaction) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto result = session.parse("begin; rollback;");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->kind(), StatementKind::transaction);
    const auto& txn = std::get<TransactCmd>(result->ast()->command);
    EXPECT_TRUE(txn.items.empty());
    EXPECT_TRUE(txn.rollback);
}

TEST(TransactParse, EmptyCommitIsSyntaxError) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    EXPECT_FALSE(session.parse("begin; commit;").has_value());
}

TEST(TransactParse, MixedReadWriteBodyIsSyntaxError) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    EXPECT_FALSE(
        session
            .parse("begin; select * from itxt1 where a = 1; delete from itxt1 where a = 2; commit;")
            .has_value());
}

TEST(TransactParse, WriteTransactionAccessAndItems) {
    ddbsh::test::FakeDDBClient client;
    ddbsh::Session session = make_session(client);
    auto result = session.parse(
        "begin; delete from itxt2 where a = 1 and b = 1; "
        "exists select * from itxt1 where a = 5; commit;");
    ASSERT_TRUE(result.has_value());
    const auto& txn = std::get<TransactCmd>(result->ast()->command);
    EXPECT_EQ(txn.access, TransactCmd::Access::write);
    EXPECT_EQ(txn.items.size(), 2u);
}

// ---- Read transactions -----------------------------------------------------

TEST(TransactRead, CommitRendersItemsOnlyNoToken) {
    ddbsh::test::FakeDDBClient client;
    register_tables(client);
    client.set_transact_get_responses(
        {num_row({{"a", "1"}, {"b", "1"}}), num_row({{"a", "2"}, {"b", "1"}})});
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "begin; select * from itxt1 where a = 1; select * from itxt1 where a = 2; commit;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "{a: 1, b: 1}\n{a: 2, b: 1}\n");
    ASSERT_EQ(client.transact_get_requests().size(), 1u);
    EXPECT_EQ(client.transact_get_requests()[0].items.size(), 2u);
}

TEST(TransactRead, EmptyRollbackRendersNothing) {
    ddbsh::test::FakeDDBClient client;
    register_tables(client);
    ddbsh::Session session = make_session(client);
    auto out = session.run("begin; rollback;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "");
    EXPECT_EQ(client.transact_get_requests().size(), 0u);
}

TEST(TransactRead, RollbackAborts) {
    ddbsh::test::FakeDDBClient client;
    register_tables(client);
    ddbsh::Session session = make_session(client);
    auto out = session.run("begin; select * from itxt1 where a = 1; rollback;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "ABORT\n");
    EXPECT_EQ(render_stderr(*out), "");
    EXPECT_EQ(client.transact_get_requests().size(), 0u);  // no API call
}

TEST(TransactRead, NonReducibleSelectAbortsWithDiagnostic) {
    ddbsh::test::FakeDDBClient client;
    register_tables(client);
    ddbsh::Session session = make_session(client);
    // a is the only key; `a=1 and b=2` is not a full-key GetItem on itxt1.
    auto out = session.run("begin; select * from itxt1 where a = 1 and b = 2; commit;");
    ASSERT_TRUE(out.has_value());  // pre-flight abort is DATA, exit 0
    EXPECT_EQ(render(*out), "ABORT\n");
    EXPECT_EQ(render_stderr(*out),
              "The SELECT cannot be used in a transaction (it cannot be reduced to a GetItem() "
              "request).\n");
    EXPECT_EQ(client.transact_get_requests().size(), 0u);
}

TEST(TransactRead, MissingResponseRendersBlankLineInRequestOrder) {
    // Fidelity (reviewer must-fix 514bb00 / D-2d §7): the original's
    // transact-read run() loop prints `show_item(item)` unconditionally for
    // every response, and show_item of an empty map is "", so a miss renders as
    // a blank line. A read transaction (unlike a SELECT) does NOT filter empty
    // responses; the positional one-line-per-Get output is the contract.
    ddbsh::test::FakeDDBClient client;
    register_tables(client);
    // Get #1 present, Get #2 a miss (nullopt), Get #3 present.
    client.set_transact_get_responses(
        {num_row({{"a", "1"}, {"b", "1"}}), std::nullopt, num_row({{"a", "3"}, {"b", "1"}})});
    ddbsh::Session session = make_session(client);

    auto out = session.run(
        "begin; select * from itxt1 where a = 1; select * from itxt1 where a = 2; "
        "select * from itxt1 where a = 3; commit;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    // Blank line for the miss, in request order (NOT skipped).
    EXPECT_EQ(render(*out), "{a: 1, b: 1}\n\n{a: 3, b: 1}\n");
}

// ---- Write transactions ----------------------------------------------------

TEST(TransactWrite, CommitRendersCommit) {
    ddbsh::test::FakeDDBClient client;
    register_tables(client);
    ddbsh::Session session = make_session(client);
    auto out = session.run(
        "begin; update balances set balance = balance + 50 where id = \"bob\"; "
        "update balances set balance = balance - 50 where id = \"alice\" and balance > 50; "
        "commit;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "COMMIT\n");
    ASSERT_EQ(client.transact_write_requests().size(), 1u);
    EXPECT_EQ(client.transact_write_requests()[0].items.size(), 2u);
}

TEST(TransactWrite, InsertAndConditionCheckCommit) {
    ddbsh::test::FakeDDBClient client;
    register_tables(client);
    ddbsh::Session session = make_session(client);
    auto out = session.run(
        "begin; not exists select * from itxt1 where a = 5; "
        "update itxt2 set flag_updated = true where a = 2 and b = 1; commit;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "COMMIT\n");
    ASSERT_EQ(client.transact_write_requests().size(), 1u);
    const auto& items = client.transact_write_requests()[0].items;
    ASSERT_EQ(items.size(), 2u);
    // First item is the ConditionCheck (not exists -> attribute_not_exists).
    ASSERT_TRUE(std::holds_alternative<ddbsh::TransactConditionCheck>(items[0]));
    const auto& cc = std::get<ddbsh::TransactConditionCheck>(items[0]);
    EXPECT_NE(cc.condition_expression.find("attribute_not_exists("), std::string::npos);
    ASSERT_TRUE(std::holds_alternative<ddbsh::TransactUpdate>(items[1]));
}

TEST(TransactWrite, ServiceCancellationAbortsThenErrors) {
    ddbsh::test::FakeDDBClient client;
    register_tables(client);
    client.set_transact_write_error(Error{
        Errc::service_error,
        "Transaction failed. TransactionCanceledException. REQ123. Transaction cancelled, please "
        "refer cancellation reasons for specific reasons [None, ConditionalCheckFailed]",
        std::string{"TransactionCanceledException"}, std::string{"REQ123"}});
    ddbsh::Session session = make_session(client);

    RecordingSink sink;
    auto rv = session.run(
        "begin; delete from itxt2 where a = 1 and b = 1; "
        "exists select * from itxt1 where a = 5; commit;",
        sink);
    // The command failed (exit nonzero) ...
    ASSERT_FALSE(rv.has_value());
    EXPECT_NE(rv.error().message.find("Transaction failed. TransactionCanceledException"),
              std::string::npos);
    // ... but the ABORT token was emitted to the sink BEFORE the error.
    ASSERT_EQ(sink.fragments.size(), 1u);
    EXPECT_EQ(render(sink.fragments[0]), "ABORT\n");
}

TEST(TransactWrite, NonFullKeyUpdateAbortsWithoutDiagnostic) {
    ddbsh::test::FakeDDBClient client;
    register_tables(client);
    ddbsh::Session session = make_session(client);
    // itxt2 needs a full (a,b) key; `a = 1` alone is not a full key.
    auto out = session.run("begin; update itxt2 set x = 1 where a = 1; commit;");
    ASSERT_TRUE(out.has_value());  // pre-flight abort, exit 0
    EXPECT_EQ(render(*out), "ABORT\n");
    EXPECT_EQ(render_stderr(*out), "");  // no diagnostic for a non-full-key UPDATE
    EXPECT_EQ(client.transact_write_requests().size(), 0u);
}

// ---- EXPLAIN ---------------------------------------------------------------

TEST(TransactExplain, CommittedReadSerializesOneBatchBlock) {
    ddbsh::test::FakeDDBClient client;
    register_tables(client);
    ddbsh::Session session = make_session(client);
    auto out = session.run("explain begin; select * from itxt1 where a = 1; commit;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    // One TransactGetItems(...) block; no API call issued.
    EXPECT_EQ(render(*out), "TransactGetItems({\n   \"stub\":   \"explain\"\n})\n");
    EXPECT_EQ(client.transact_get_requests().size(), 0u);
    ASSERT_EQ(client.explain_requests().size(), 1u);
    EXPECT_TRUE(std::holds_alternative<ddbsh::TransactGetItemsIn>(client.explain_requests()[0]));
}

TEST(TransactExplain, CommittedWriteSerializesOneBatchBlock) {
    ddbsh::test::FakeDDBClient client;
    register_tables(client);
    ddbsh::Session session = make_session(client);
    auto out = session.run("explain begin; delete from itxt1 where a = 1; commit;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "TransactWriteItems({\n   \"stub\":   \"explain\"\n})\n");
    ASSERT_EQ(client.explain_requests().size(), 1u);
    EXPECT_TRUE(std::holds_alternative<ddbsh::TransactWriteItemsIn>(client.explain_requests()[0]));
}

TEST(TransactExplain, RolledBackSerializesNothingAndAborts) {
    ddbsh::test::FakeDDBClient client;
    register_tables(client);
    ddbsh::Session session = make_session(client);
    auto out = session.run("explain begin; select * from itxt1 where a = 1; rollback;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "ABORT\n");
    EXPECT_EQ(client.explain_requests().size(), 0u);
}

}  // namespace

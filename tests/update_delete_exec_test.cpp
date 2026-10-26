// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// update_delete_exec_test.cpp — 2c UPDATE/UPSERT + DELETE + streaming tests.
//
// Exercises the mutating-command paths offline via FakeDDBClient:
//   * UPDATE/UPSERT/DELETE single-conditional-write (full base-table key):
//     one UpdateItem/DeleteItem with the condition; BulkWriteOutcome
//     (0 read, 1 modified, 0 ccf). UPDATE prepends attribute_exists(pk);
//     UPSERT does not.
//   * Bulk Query/Scan enumeration -> per-item write; read/modified accounting.
//   * ConditionalCheckFailed is a ccf counter increment (DATA), not an error
//     (D-2c-7/-8); any other service error aborts without the accounting line.
//   * The write-request alias prefix is minted BEFORE the enumeration read's
//     prefix (D-2c-10): write tokens use "aaa", the read uses "baa".
//   * The enumeration projects only the base-table key(s) (D-2c-9).
//   * EXPLAIN: single -> one write block; UPDATE bulk -> read block + per-item
//     write blocks; DELETE query -> Query block ONLY (no DeleteItem), DELETE
//     scan -> Scan block + per-item DeleteItem (the §8.3 asymmetry).
//   * Streaming: multi-row INSERT emits one WriteOutcome per written row before
//     a mid-batch failure's Error (AI-4 / D-2c-12); the buffering execute folds
//     >1 fragment into a MultiOutcome.

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/outcome_sink.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "fake_ddb_client.hpp"

namespace {

using ddbsh::DescribeTableOut;
using ddbsh::Errc;
using ddbsh::Error;
using ddbsh::ExecOutcome;
using ddbsh::ItemRow;
using ddbsh::SecondaryIndexSchema;
using ddbsh::TableKeySchema;
using ddbsh::Value;

DescribeTableOut table_pk(const std::string& name, const std::string& pk,
                          const std::string& rk = "") {
    DescribeTableOut t;
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

ItemRow key_row(std::vector<std::pair<std::string, std::string>> numbers) {
    ItemRow row;
    for (auto& [name, n] : numbers) {
        row.attributes.emplace_back(name, Value::number_value(n));
    }
    return row;
}

// A recording sink for streaming-path assertions.
struct RecordingSink : ddbsh::OutcomeSink {
    std::vector<ExecOutcome> fragments;
    void emit(const ExecOutcome& fragment) override { fragments.push_back(fragment); }
};

// ---- UPDATE: single conditional write --------------------------------------

TEST(UpdateExec, SingleFullKeyUpdateItemWithAttributeExistsCondition) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("iut1", "pk"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("update iut1 set y = 14 where pk = 0;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "UPDATE (0 read, 1 modified, 0 ccf)\n");

    // No enumeration read; exactly one UpdateItem.
    EXPECT_EQ(client.read_calls(), 0);
    ASSERT_EQ(client.update_requests().size(), 1u);
    const auto& req = client.update_requests()[0];
    EXPECT_EQ(req.table_name, "iut1");
    EXPECT_EQ(req.update_expression, "SET #aaaa1 = :vaaa1");
    ASSERT_TRUE(req.condition_expression.has_value());
    EXPECT_EQ(*req.condition_expression, "attribute_exists(#aaaa2)");
    // Key is the literal gud key {pk: 0}.
    ASSERT_EQ(req.key.size(), 1u);
    EXPECT_EQ(req.key[0].first, "pk");
    EXPECT_EQ(req.key[0].second.s, "0");
    // EAN sorted by token: #aaaa1->y, #aaaa2->pk.
    ASSERT_EQ(req.expression_attribute_names.size(), 2u);
    EXPECT_EQ(req.expression_attribute_names[0].second, "y");
    EXPECT_EQ(req.expression_attribute_names[1].second, "pk");
}

TEST(UpdateExec, UpsertSingleOmitsAttributeExists) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("iut1", "pk"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("upsert iut1 set y = 14 where pk = 0;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "UPSERT (0 read, 1 modified, 0 ccf)\n");
    ASSERT_EQ(client.update_requests().size(), 1u);
    // UPSERT has no attribute_exists(pk) condition (and no residual here).
    EXPECT_FALSE(client.update_requests()[0].condition_expression.has_value());
}

TEST(UpdateExec, SingleResidualPredicateReassertsInCondition) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("iut1", "pk"));
    ddbsh::Session session = make_session(client);

    // Full key + extra predicate: the extra predicate becomes the condition
    // tail (optimistic concurrency), after attribute_exists(pk).
    auto out = session.run("update iut1 set x = 3 where pk = 0 and y = 14;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    ASSERT_EQ(client.update_requests().size(), 1u);
    EXPECT_EQ(*client.update_requests()[0].condition_expression,
              "attribute_exists(#aaaa2) AND #aaaa3 = :vaaa2");
}

TEST(UpdateExec, RemoveBuildsRemoveExpression) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("iut1", "pk"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("update iut1 remove y where pk = 0;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    ASSERT_EQ(client.update_requests().size(), 1u);
    EXPECT_EQ(client.update_requests()[0].update_expression, "REMOVE #aaaa1");
    EXPECT_EQ(*client.update_requests()[0].condition_expression, "attribute_exists(#aaaa2)");
}

TEST(UpdateExec, IfNotExistsSetForm) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("iut1", "pk"));
    ddbsh::Session session = make_session(client);

    // if_not_exists is FIDELITY (golden iut-ifne-1): SET #a = if_not_exists(#a,:v).
    auto out = session.run("update iut1 set x = if_not_exists(x, 5) where pk = 1;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    ASSERT_EQ(client.update_requests().size(), 1u);
    EXPECT_EQ(client.update_requests()[0].update_expression,
              "SET #aaaa1 = if_not_exists(#aaaa1,:vaaa1)");
    EXPECT_EQ(*client.update_requests()[0].condition_expression, "attribute_exists(#aaaa2)");
}

// ---- UPDATE: bulk enumeration ----------------------------------------------

TEST(UpdateExec, BulkScanWritePrefixBeforeReadPrefix) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("iut1", "pk"));
    client.set_items({key_row({{"pk", "0"}}), key_row({{"pk", "1"}})});
    ddbsh::Session session = make_session(client);

    // y is not a key => Scan enumeration, then per-item UpdateItem.
    auto out = session.run("update iut1 set x = 3 where y = 14;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "UPDATE (2 read, 2 modified, 0 ccf)\n");

    // Write request uses the FIRST prefix (aaa); the Scan the NEXT (baa).
    ASSERT_EQ(client.update_requests().size(), 2u);
    EXPECT_EQ(client.update_requests()[0].update_expression, "SET #aaaa1 = :vaaa1");
    EXPECT_EQ(*client.update_requests()[0].condition_expression,
              "attribute_exists(#aaaa2) AND #aaaa3 = :vaaa2");
    // Per-item keys come from the enumerated rows.
    EXPECT_EQ(client.update_requests()[0].key[0].second.s, "0");
    EXPECT_EQ(client.update_requests()[1].key[0].second.s, "1");
}

TEST(UpdateExec, BulkQueryOnPartitionKeyEquality) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("iut2", "pk", "rk"));
    client.set_items({key_row({{"pk", "2"}, {"rk", "3"}}), key_row({{"pk", "2"}, {"rk", "4"}})});
    ddbsh::Session session = make_session(client);

    // pk = 2 (no rk) => Query enumeration on the base table.
    auto out = session.run("update iut2 set y = 14 where pk = 2;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "UPDATE (2 read, 2 modified, 0 ccf)\n");
    EXPECT_EQ(client.read_calls(), 1);
    ASSERT_EQ(client.update_requests().size(), 2u);
    // Both-key item keys addressed to the base table.
    EXPECT_EQ(client.update_requests()[0].key.size(), 2u);
}

TEST(UpdateExec, ConditionalCheckFailedCountsAsCcfNotError) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("iut1", "pk"));
    client.set_update_error(Error{Errc::conditional_check_failed, "The conditional request failed",
                                  std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);

    auto out = session.run("update iut1 set y = 14 where pk = 0;");
    ASSERT_TRUE(out.has_value()) << "ccf must not abort the command";
    EXPECT_EQ(render(*out), "UPDATE (0 read, 0 modified, 1 ccf)\n");
}

TEST(UpdateExec, NonCcfServiceErrorAborts) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("iut1", "pk"));
    client.set_update_error(
        Error{Errc::service_error, "ProvisionedThroughputExceeded", std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);

    auto out = session.run("update iut1 set y = 14 where pk = 0;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code, Errc::service_error);
}

// ---- UPDATE: EXPLAIN -------------------------------------------------------

TEST(UpdateExplain, SingleEmitsOneUpdateItemBlock) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("iut1", "pk"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("explain update iut1 set y = 14 where pk = 0;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    // One UpdateItem block; no mutation issued.
    EXPECT_TRUE(client.update_requests().empty());
    const auto& explain = std::get<ddbsh::ExplainOutcome>(*out);
    ASSERT_EQ(explain.blocks.size(), 1u);
    EXPECT_EQ(explain.blocks[0].api_name, "UpdateItem");
}

TEST(UpdateExplain, BulkScanEmitsScanThenPerItemUpdateBlocks) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("iut1", "pk"));
    client.set_items({key_row({{"pk", "0"}}), key_row({{"pk", "1"}})});
    ddbsh::Session session = make_session(client);

    auto out = session.run("explain update iut1 set x = 3 where y = 14;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_TRUE(client.update_requests().empty());  // no mutation
    const auto& explain = std::get<ddbsh::ExplainOutcome>(*out);
    ASSERT_EQ(explain.blocks.size(), 3u);
    EXPECT_EQ(explain.blocks[0].api_name, "Scan");
    EXPECT_EQ(explain.blocks[1].api_name, "UpdateItem");
    EXPECT_EQ(explain.blocks[2].api_name, "UpdateItem");
}

// ---- DELETE ----------------------------------------------------------------

TEST(DeleteExec, SingleFullKeyDeleteItem) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("idt1", "pk"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("delete from idt1 where pk = 0;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "DELETE (0 read, 1 modified, 0 ccf)\n");
    ASSERT_EQ(client.delete_item_requests().size(), 1u);
    // No attribute_exists condition for DELETE (and no residual here).
    EXPECT_FALSE(client.delete_item_requests()[0].condition_expression.has_value());
    EXPECT_EQ(client.delete_item_requests()[0].key[0].second.s, "0");
}

TEST(DeleteExec, NoWhereScansAndDeletesAll) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("idt1", "pk"));
    client.set_items({key_row({{"pk", "0"}}), key_row({{"pk", "1"}}), key_row({{"pk", "2"}})});
    ddbsh::Session session = make_session(client);

    auto out = session.run("delete from idt1;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "DELETE (3 read, 3 modified, 0 ccf)\n");
    EXPECT_EQ(client.delete_item_requests().size(), 3u);
}

TEST(DeleteExec, BulkScanWritePrefixBeforeReadPrefixAndBaseKeyProjection) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("idt2", "pk", "rk"));
    client.set_items({key_row({{"pk", "3"}, {"rk", "6"}})});
    ddbsh::Session session = make_session(client);

    // rk = 6 (no pk equality) => Scan; projection is the base keys pk, rk.
    auto out = session.run("delete from idt2 where rk = 6;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "DELETE (1 read, 1 modified, 0 ccf)\n");
    ASSERT_EQ(client.delete_item_requests().size(), 1u);
    EXPECT_EQ(client.delete_item_requests()[0].key.size(), 2u);
}

TEST(DeleteExec, ConditionalCheckFailedCountsAsCcf) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("idt1", "pk"));
    client.set_delete_item_error(Error{Errc::conditional_check_failed,
                                       "The conditional request failed", std::nullopt,
                                       std::nullopt});
    ddbsh::Session session = make_session(client);

    auto out = session.run("delete from idt1 where pk = 0;");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(render(*out), "DELETE (0 read, 0 modified, 1 ccf)\n");
}

// ---- DELETE: EXPLAIN asymmetry (§8.3) --------------------------------------

TEST(DeleteExplain, QueryPathEmitsQueryBlockOnlyAndDoesNotRun) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("idt2", "pk", "rk"));
    client.set_items({key_row({{"pk", "2"}, {"rk", "3"}})});
    ddbsh::Session session = make_session(client);

    // pk = 2 => Query path. EXPLAIN DELETE via query emits Query ONLY.
    auto out = session.run("explain delete from idt2 where pk = 2;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(client.read_calls(), 0);  // the query is NOT run
    const auto& explain = std::get<ddbsh::ExplainOutcome>(*out);
    ASSERT_EQ(explain.blocks.size(), 1u);
    EXPECT_EQ(explain.blocks[0].api_name, "Query");
}

TEST(DeleteExplain, ScanPathEmitsScanThenPerItemDeleteBlocks) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("idt2", "pk", "rk"));
    client.set_items({key_row({{"pk", "3"}, {"rk", "6"}}), key_row({{"pk", "4"}, {"rk", "6"}})});
    ddbsh::Session session = make_session(client);

    // rk = 6 => Scan path. EXPLAIN DELETE via scan RUNS the scan and emits a
    // DeleteItem block per returned item (§8.3).
    auto out = session.run("explain delete from idt2 where rk = 6;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(client.read_calls(), 1);
    const auto& explain = std::get<ddbsh::ExplainOutcome>(*out);
    ASSERT_EQ(explain.blocks.size(), 3u);
    EXPECT_EQ(explain.blocks[0].api_name, "Scan");
    EXPECT_EQ(explain.blocks[1].api_name, "DeleteItem");
    EXPECT_EQ(explain.blocks[2].api_name, "DeleteItem");
}

// ---- Streaming sink + MultiOutcome (2c-1) ----------------------------------

TEST(StreamingInsert, EmitsOneFragmentPerRow) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "id"));
    ddbsh::Session session = make_session(client);

    auto stmt = session.parse("insert into t ( id ) values ( 1 ), ( 2 ), ( 3 );");
    ASSERT_TRUE(stmt.has_value());
    RecordingSink sink;
    auto result = session.execute(*stmt, sink);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(sink.fragments.size(), 3u);
    for (const auto& f : sink.fragments) {
        EXPECT_EQ(render(f), "INSERT\n");
    }
}

TEST(StreamingInsert, PartialOutputBeforeMidBatchFailure) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "id"));
    // Fail the 2nd row (0-based index 1); row 1 must still be emitted.
    client.set_put_error_at(1, Error{Errc::conditional_check_failed,
                                     "The conditional request failed", std::nullopt, std::nullopt});
    ddbsh::Session session = make_session(client);

    auto stmt = session.parse("insert into t ( id ) values ( 1 ), ( 2 ), ( 3 );");
    ASSERT_TRUE(stmt.has_value());
    RecordingSink sink;
    auto result = session.execute(*stmt, sink);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, Errc::conditional_check_failed);
    // Only row 1 was emitted before the row-2 failure stopped the batch.
    ASSERT_EQ(sink.fragments.size(), 1u);
    EXPECT_EQ(render(sink.fragments[0]), "INSERT\n");
    // Row 3 was never attempted.
    EXPECT_EQ(client.put_requests().size(), 2u);
}

TEST(StreamingInsert, BufferingExecuteFoldsMultiRowIntoMultiOutcome) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("t", "id"));
    ddbsh::Session session = make_session(client);

    // The buffering execute() collects >1 fragment into a MultiOutcome whose
    // render concatenates each part.
    auto out = session.run("insert into t ( id ) values ( 1 ), ( 2 );");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    ASSERT_TRUE(std::holds_alternative<ddbsh::MultiOutcome>(*out));
    EXPECT_EQ(render(*out), "INSERT\nINSERT\n");
}

// ---- Vector attribute values (phase 4b, P4b-4) -----------------------------

TEST(UpdateExec, SetVectorAttributeRoundTripsToListOfNumbers) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_pk("docs", "id"));
    ddbsh::Session session = make_session(client);

    auto out = session.run("update docs set embedding = [0.9, 0.1] where id = \"d1\";");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    ASSERT_EQ(client.update_requests().size(), 1u);
    const auto& values = client.update_requests()[0].expression_attribute_values;
    ASSERT_FALSE(values.empty());
    const ddbsh::Value& v = values[0].second;
    EXPECT_EQ(v.type, ddbsh::Value::Type::L);
    ASSERT_EQ(v.list.size(), 2u);
    EXPECT_EQ(v.list[0].type, ddbsh::Value::Type::N);
    EXPECT_EQ(v.list[0].s, "0.9");
    EXPECT_EQ(v.list[1].s, "0.1");
}

// ---- Bulk write routed through a composite GSI (phase 4b backlog, G2) ------

DescribeTableOut table_with_composite_gsi(const std::string& name) {
    DescribeTableOut t;
    t.table_name = name;
    t.key_schema = ddbsh::test::make_key_schema("id");
    SecondaryIndexSchema gsi;
    gsi.index_name = "g";
    gsi.key_schema.hash_keys = {"a", "b", "c"};
    t.global_secondary_indexes.push_back(gsi);
    return t;
}

TEST(UpdateExec, BulkUpdateThroughCompositeGsiEnumeratesViaIndex) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_with_composite_gsi("docs"));
    // Enumerated rows carry the base-table key (id) so per-item UpdateItem can
    // address the base table (the read is routed through the index).
    client.set_items({key_row({{"id", "1"}}), key_row({{"id", "2"}})});
    ddbsh::Session session = make_session(client);

    // Full composite hash on the GSI => Query enumeration through index g.
    auto out = session.run("update docs.g set score = 9 where a = 3 and b = 4 and c = 5;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "UPDATE (2 read, 2 modified, 0 ccf)\n");
    EXPECT_EQ(client.read_calls(), 1);  // one Query page through the index
    ASSERT_EQ(client.update_requests().size(), 2u);
    // Per-item writes hit the base table, keyed by the enumerated base key.
    EXPECT_EQ(client.update_requests()[0].table_name, "docs");
    EXPECT_EQ(client.update_requests()[0].key[0].first, "id");
}

TEST(DeleteExec, BulkDeleteThroughCompositeGsiEnumeratesViaIndex) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_with_composite_gsi("docs"));
    client.set_items({key_row({{"id", "1"}})});
    ddbsh::Session session = make_session(client);

    // Partial composite hash (a only) => Scan enumeration through index g.
    auto out = session.run("delete from docs.g where a = 3;");
    ASSERT_TRUE(out.has_value()) << (out.has_value() ? "" : out.error().message);
    EXPECT_EQ(render(*out), "DELETE (1 read, 1 modified, 0 ccf)\n");
    ASSERT_EQ(client.delete_item_requests().size(), 1u);
    EXPECT_EQ(client.delete_item_requests()[0].table_name, "docs");
    EXPECT_EQ(client.delete_item_requests()[0].key[0].first, "id");
}

}  // namespace

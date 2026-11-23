// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// select_stream_test.cpp — D-P5-1 (phase 5b) lazy/streaming SELECT tests.
//
// Exercises the streaming SELECT path (Session::execute(stmt, sink)) offline
// via FakeDDBClient's canned multi-page reads:
//   * one SelectOutcome fragment is emitted PER NON-EMPTY PAGE (bounded memory
//     — the whole result is never buffered in the engine);
//   * empty pages are drained (capacity totalized, no spurious fragment);
//   * consumed capacity is totalized across pages and emitted as a FINAL
//     capacity-only fragment, only when the user asked (RETURN CONSUMED
//     CAPACITY);
//   * the concatenation of the streamed fragments renders BYTE-IDENTICALLY to
//     the single buffered SelectOutcome the buffering overload produces — the
//     705/705 golden fidelity contract (D-P5-A7).
// The embedding-side lazy next_row()/paging is covered by memory_client_test
// (PagedScanMatchesUnpagedBytes / PagedQueryCrossesBoundaries), which now drive
// the same cursor through ddbsh::Connection.

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/outcome_sink.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/session.hpp"

#include "fake_ddb_client.hpp"

namespace {

using ddbsh::ConsumedCapacity;
using ddbsh::DescribeTableOut;
using ddbsh::ExecOutcome;
using ddbsh::ItemRow;
using ddbsh::SelectOutcome;
using ddbsh::TableKeySchema;
using ddbsh::Value;
using ddbsh::ValueMap;

DescribeTableOut table_a(const std::string& name) {
    DescribeTableOut t;
    t.table_name = name;
    t.key_schema = TableKeySchema{{"a"}, {}};
    return t;
}

ItemRow row(ValueMap attrs) {
    return ItemRow{std::move(attrs)};
}

Value num(const std::string& n) {
    return Value::number_value(n);
}

ddbsh::Session make_session(ddbsh::test::FakeDDBClient& client) {
    return ddbsh::Session(ddbsh::SessionConfig{"us-east-1", ""}, client);
}

// A sink that records each emitted fragment (a copy), for structural asserts.
struct RecordingSink : ddbsh::OutcomeSink {
    std::vector<ExecOutcome> parts;
    void emit(const ExecOutcome& fragment) override { parts.push_back(fragment); }
};

std::string render_all(const std::vector<ExecOutcome>& parts) {
    ddbsh::render::Renderer renderer;
    std::string out;
    for (const auto& p : parts) {
        out += renderer.render(p);
    }
    return out;
}

// Three pages of two numbered rows each: a, c attributes so rendering is
// deterministic and sort-independent for equality.
std::vector<std::vector<ItemRow>> three_pages() {
    return {
        {row({{"a", num("1")}}), row({{"a", num("2")}})},
        {row({{"a", num("3")}}), row({{"a", num("4")}})},
        {row({{"a", num("5")}}), row({{"a", num("6")}})},
    };
}

// ---- Per-page streaming ----------------------------------------------------

TEST(SelectStream, ScanEmitsOneFragmentPerPage) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a("t"));
    client.set_pages(three_pages());
    ddbsh::Session session = make_session(client);

    auto stmt = session.parse("select * from t;");  // no WHERE => scan
    ASSERT_TRUE(stmt.has_value());

    RecordingSink sink;
    auto res = session.execute(*stmt, sink);
    ASSERT_TRUE(res.has_value());

    // One fragment per page (no capacity fragment: the user did not ask).
    ASSERT_EQ(sink.parts.size(), 3u);
    EXPECT_EQ(client.read_calls(), 3);
    for (const auto& part : sink.parts) {
        const auto* s = std::get_if<SelectOutcome>(&part);
        ASSERT_NE(s, nullptr);
        EXPECT_EQ(s->items.size(), 2u);                  // the page's rows
        EXPECT_FALSE(s->consumed_capacity.has_value());  // no cc on item pages
    }
    EXPECT_EQ(render_all(sink.parts), "{a: 1}\n{a: 2}\n{a: 3}\n{a: 4}\n{a: 5}\n{a: 6}\n");
}

TEST(SelectStream, EmptyMiddlePageIsDrainedNotEmitted) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a("t"));
    client.set_pages({
        {row({{"a", num("1")}})},
        {},  // empty page in the middle (still paged over)
        {row({{"a", num("2")}})},
    });
    ddbsh::Session session = make_session(client);

    auto stmt = session.parse("select * from t;");
    ASSERT_TRUE(stmt.has_value());

    RecordingSink sink;
    ASSERT_TRUE(session.execute(*stmt, sink).has_value());

    // Three reads happened, but only the two non-empty pages produced output.
    EXPECT_EQ(client.read_calls(), 3);
    ASSERT_EQ(sink.parts.size(), 2u);
    EXPECT_EQ(render_all(sink.parts), "{a: 1}\n{a: 2}\n");
}

TEST(SelectStream, EmptyResultEmitsNoFragments) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a("t"));
    client.set_pages({{}});  // one empty page, no cursor
    ddbsh::Session session = make_session(client);

    auto stmt = session.parse("select * from t;");
    ASSERT_TRUE(stmt.has_value());

    RecordingSink sink;
    ASSERT_TRUE(session.execute(*stmt, sink).has_value());

    EXPECT_EQ(client.read_calls(), 1);
    EXPECT_TRUE(sink.parts.empty());
    EXPECT_EQ(render_all(sink.parts), "");
}

TEST(SelectStream, GetItemEmitsSingleFragment) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a("t"));
    client.set_get_item(row({{"a", num("2")}, {"c", num("9")}}));
    ddbsh::Session session = make_session(client);

    auto stmt = session.parse("select * from t where a = 2;");  // full key => GetItem
    ASSERT_TRUE(stmt.has_value());

    RecordingSink sink;
    ASSERT_TRUE(session.execute(*stmt, sink).has_value());

    ASSERT_EQ(sink.parts.size(), 1u);
    EXPECT_EQ(render_all(sink.parts), "{a: 2, c: 9}\n");
}

// ---- Consumed capacity totalized into a final fragment ---------------------

TEST(SelectStream, TotalizesCapacityIntoFinalFragment) {
    ddbsh::test::FakeDDBClient client;
    client.set_table(table_a("t"));
    client.set_pages(three_pages());
    ConsumedCapacity cc;
    cc.table_name = "t";
    cc.capacity_units = 1.0;  // per page => 3.0 totalized
    client.set_consumed_capacity(cc);
    ddbsh::Session session = make_session(client);

    auto stmt = session.parse("select * from t return consumed capacity total;");
    ASSERT_TRUE(stmt.has_value());

    RecordingSink sink;
    ASSERT_TRUE(session.execute(*stmt, sink).has_value());

    // 3 page fragments + 1 capacity-only fragment.
    ASSERT_EQ(sink.parts.size(), 4u);
    for (std::size_t i = 0; i < 3; ++i) {
        const auto* s = std::get_if<SelectOutcome>(&sink.parts[i]);
        ASSERT_NE(s, nullptr);
        EXPECT_FALSE(s->consumed_capacity.has_value());
        EXPECT_EQ(s->items.size(), 2u);
    }
    const auto* last = std::get_if<SelectOutcome>(&sink.parts[3]);
    ASSERT_NE(last, nullptr);
    EXPECT_TRUE(last->items.empty());
    ASSERT_TRUE(last->consumed_capacity.has_value());
    EXPECT_EQ(last->consumed_capacity->capacity_units, 3.0);  // 1.0 * 3 pages

    EXPECT_EQ(render_all(sink.parts),
              "{a: 1}\n{a: 2}\n{a: 3}\n{a: 4}\n{a: 5}\n{a: 6}\nSELECT (t, 3, 0, 0)\n");
}

// ---- Byte-identicality vs the buffering overload (the fidelity contract) ---

TEST(SelectStream, StreamedBytesEqualBufferedBytes) {
    // Build two identically-configured clients so read_calls / canned data are
    // independent between the streamed and buffered runs.
    const auto configure = [](ddbsh::test::FakeDDBClient& c) {
        c.set_table(table_a("t"));
        c.set_pages(three_pages());
        ConsumedCapacity cc;
        cc.table_name = "t";
        cc.capacity_units = 2.5;
        c.set_consumed_capacity(cc);
    };

    ddbsh::test::FakeDDBClient streamed_client;
    configure(streamed_client);
    ddbsh::Session streamed_session = make_session(streamed_client);
    auto stmt_s = streamed_session.parse("select * from t return consumed capacity total;");
    ASSERT_TRUE(stmt_s.has_value());
    RecordingSink sink;
    ASSERT_TRUE(streamed_session.execute(*stmt_s, sink).has_value());
    std::string streamed = render_all(sink.parts);

    ddbsh::test::FakeDDBClient buffered_client;
    configure(buffered_client);
    ddbsh::Session buffered_session = make_session(buffered_client);
    auto buffered_out = buffered_session.run("select * from t return consumed capacity total;");
    ASSERT_TRUE(buffered_out.has_value());
    std::string buffered = ddbsh::render::Renderer{}.render(*buffered_out);

    EXPECT_EQ(streamed, buffered);
}

}  // namespace

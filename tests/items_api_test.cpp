// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// items_api_test.cpp — the result-consumption convenience API (D11).
//
// Covers the three additive, SDK-free helpers that let an embedder consume
// SELECT results without digging through the ExecOutcome variant or
// reimplementing the Value::Type switch:
//   * ddbsh::render::render_value(const Value&)  — one cell to its ddbsh string
//   * ddbsh::ItemRow::get(std::string_view)      — typed single-attribute lookup
//   * ddbsh::items(const ExecOutcome&)           — row extraction across arms
// These are pure functions, so the tests build values/outcomes directly — no
// Session, no client, no backend.

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ddbsh/exec_outcome.hpp"
#include "ddbsh/iddbclient.hpp"
#include "ddbsh/renderer.hpp"
#include "ddbsh/value.hpp"

namespace {

using ddbsh::ItemRow;
using ddbsh::Value;
using ddbsh::render::render_value;

ItemRow movie(std::string year, std::string title, std::string rating) {
    ItemRow row;
    row.attributes.emplace_back("year", Value::number_value(std::move(year)));
    row.attributes.emplace_back("title", Value::string_value(std::move(title)));
    row.attributes.emplace_back("rating", Value::number_value(std::move(rating)));
    return row;
}

// ---- render_value: one implementation, every Value::Type ------------------

TEST(RenderValue, SimpleStringIsBare) {
    EXPECT_EQ(render_value(Value::string_value("Inception")), "Inception");
}

TEST(RenderValue, StringWithSpecialCharsIsQuoted) {
    // A space/`,`/`:`/bracket/brace forces quoting (quote_if_required, arch §9).
    EXPECT_EQ(render_value(Value::string_value("hello world")), "\"hello world\"");
    EXPECT_EQ(render_value(Value::string_value("a,b")), "\"a,b\"");
    EXPECT_EQ(render_value(Value::string_value("k:v")), "\"k:v\"");
}

TEST(RenderValue, NumberIsBareAndExact) {
    // N is stored as the exact string — no double round-trip (arch §: value.hpp).
    EXPECT_EQ(render_value(Value::number_value("8.60")), "8.60");
    EXPECT_EQ(render_value(Value::number_value("-0")), "-0");
}

TEST(RenderValue, BooleanIsUppercase) {
    EXPECT_EQ(render_value(Value::bool_value(true)), "TRUE");
    EXPECT_EQ(render_value(Value::bool_value(false)), "FALSE");
}

TEST(RenderValue, NullIsUppercase) {
    EXPECT_EQ(render_value(Value::null_value()), "NULL");
}

TEST(RenderValue, ListRendersBracketed) {
    Value l;
    l.type = Value::Type::L;
    l.list.push_back(Value::number_value("3"));
    l.list.push_back(Value::number_value("4"));
    l.list.push_back(Value::string_value("hello"));
    EXPECT_EQ(render_value(l), "[3, 4, hello]");
}

TEST(RenderValue, EmptyListAndMap) {
    Value l;
    l.type = Value::Type::L;
    EXPECT_EQ(render_value(l), "[]");
    Value m;
    m.type = Value::Type::M;
    EXPECT_EQ(render_value(m), "{}");
}

TEST(RenderValue, NestedMapSortsKeysAndOmitsSpaceAfterColon) {
    // Nested map: keys alpha-sorted, NO space after the colon (contrast the
    // top-level item `: `) — arch §9. Build in non-sorted order to prove the
    // sort.
    Value m;
    m.type = Value::Type::M;
    m.map.emplace_back("c", Value::string_value("hello"));
    m.map.emplace_back("a", Value::number_value("36"));
    m.map.emplace_back("b", Value::number_value("36"));
    EXPECT_EQ(render_value(m), "{a:36, b:36, c:hello}");
}

TEST(RenderValue, UnsupportedTypeRendersUnknown) {
    // Sets (SS/NS/BS) and binary (B) are not reachable via the in-scope
    // commands; the original rendered them "Unknown Type".
    Value ss;
    ss.type = Value::Type::SS;
    ss.set = {"a", "b"};
    EXPECT_EQ(render_value(ss), "Unknown Type");
}

// ---- ItemRow::get ----------------------------------------------------------

TEST(ItemRowGet, PresentReturnsValue) {
    ItemRow row = movie("2010", "Inception", "8.8");
    const std::optional<Value> title = row.get("title");
    ASSERT_TRUE(title.has_value());
    EXPECT_EQ(title->type, Value::Type::S);
    EXPECT_EQ(title->s, "Inception");
}

TEST(ItemRowGet, AbsentReturnsNullopt) {
    ItemRow row = movie("2010", "Inception", "8.8");
    EXPECT_FALSE(row.get("director").has_value());
}

TEST(ItemRowGet, EmptyRowReturnsNullopt) {
    ItemRow row;
    EXPECT_FALSE(row.get("anything").has_value());
}

// ---- ddbsh::items ----------------------------------------------------------

TEST(Items, SelectOutcomeReturnsRows) {
    ddbsh::SelectOutcome sel;
    sel.items.push_back(movie("2010", "Inception", "8.8"));
    sel.items.push_back(movie("2014", "Interstellar", "8.6"));
    ddbsh::ExecOutcome outcome = sel;

    const std::vector<ItemRow> rows = ddbsh::items(outcome);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].get("title")->s, "Inception");
    EXPECT_EQ(rows[1].get("title")->s, "Interstellar");
}

TEST(Items, TransactReadOutcomeReturnsRowsIncludingMisses) {
    ddbsh::TransactReadOutcome tr;
    tr.items.push_back(movie("2010", "Inception", "8.8"));
    tr.items.push_back(ItemRow{});  // a miss is an empty row, preserved positionally
    ddbsh::ExecOutcome outcome = tr;

    const std::vector<ItemRow> rows = ddbsh::items(outcome);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_TRUE(rows[0].get("title").has_value());
    EXPECT_TRUE(rows[1].attributes.empty());
}

TEST(Items, VectorSearchOutcomeReturnsItemsWithoutScore) {
    ddbsh::VectorSearchOutcome vs;
    vs.rows.push_back(ddbsh::VectorSearchOutcome::Row{movie("2010", "Inception", "8.8"), 0.97});
    vs.rows.push_back(ddbsh::VectorSearchOutcome::Row{movie("2014", "Interstellar", "8.6"), 0.81});
    ddbsh::ExecOutcome outcome = vs;

    const std::vector<ItemRow> rows = ddbsh::items(outcome);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].get("title")->s, "Inception");
    // The score is NOT folded in as an attribute.
    EXPECT_FALSE(rows[0].get("score").has_value());
}

TEST(Items, MultiOutcomeFlattensInOrder) {
    ddbsh::SelectOutcome a;
    a.items.push_back(movie("2010", "Inception", "8.8"));
    ddbsh::SelectOutcome b;
    b.items.push_back(movie("2014", "Interstellar", "8.6"));
    ddbsh::MultiOutcome multi;
    multi.parts.push_back(ddbsh::ExecOutcome{a});
    multi.parts.push_back(ddbsh::ExecOutcome{b});
    ddbsh::ExecOutcome outcome = multi;

    const std::vector<ItemRow> rows = ddbsh::items(outcome);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].get("title")->s, "Inception");
    EXPECT_EQ(rows[1].get("title")->s, "Interstellar");
}

TEST(Items, NonRowArmsReturnEmpty) {
    EXPECT_TRUE(ddbsh::items(ddbsh::ExecOutcome{ddbsh::StatusOutcome{"CREATE", {}}}).empty());
    EXPECT_TRUE(ddbsh::items(ddbsh::ExecOutcome{ddbsh::EmptyOutcome{}}).empty());
    EXPECT_TRUE(ddbsh::items(ddbsh::ExecOutcome{ddbsh::ReportOutcome{{"a", "b"}}}).empty());
}

TEST(Items, EmptySelectReturnsEmpty) {
    EXPECT_TRUE(ddbsh::items(ddbsh::ExecOutcome{ddbsh::SelectOutcome{}}).empty());
}

}  // namespace

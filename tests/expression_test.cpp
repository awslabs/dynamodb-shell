// Copyright 2026 ddbsh reimplementation contributors.
// SPDX-License-Identifier: Apache-2.0
//
// expression_test.cpp — unit tests for the SDK-free expression engine (W3
// part 1): the deterministic alias prefix, the symbol table, the logical
// expression tree's serialization, and the Where capability probes that drive
// the GetItem/Query/Scan decision (arch §6, §8). These are fully offline and
// deterministic — the richest fidelity signal before the SELECT grammar and
// planner land (W3 part 2 / W4).

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "ddbsh/value.hpp"
#include "expressions/logical_and.hpp"
#include "expressions/logical_comparison.hpp"
#include "expressions/logical_filter_predicates.hpp"
#include "expressions/logical_or.hpp"
#include "expressions/logical_range_predicates.hpp"
#include "expressions/prefix.hpp"
#include "expressions/symbol_table.hpp"
#include "expressions/where.hpp"

using namespace ddbsh;
using namespace ddbsh::engine;

namespace {

std::unique_ptr<LogicalExpression> eq(const std::string& lhs, const std::string& op, Value rhs) {
    return std::make_unique<LogicalComparison>(lhs, op, std::move(rhs));
}

Value num(const std::string& n) {
    return Value::number_value(n);
}
Value str(const std::string& s) {
    return Value::string_value(s);
}

}  // namespace

// ---- prefix generator (arch §14 item 1) ------------------------------------

TEST(Prefix, MatchesOriginalBase52Sequence) {
    EXPECT_EQ(unique_prefix(0), "aaa");
    EXPECT_EQ(unique_prefix(1), "baa");  // little-endian: least significant first
    EXPECT_EQ(unique_prefix(25), "zaa");
    EXPECT_EQ(unique_prefix(26), "Aaa");  // 'A' is index 26
    EXPECT_EQ(unique_prefix(51), "Zaa");
    EXPECT_EQ(unique_prefix(52), "aba");  // carries into the second position
    EXPECT_EQ(unique_prefix(53), "bba");
}

// ---- symbol table ----------------------------------------------------------

TEST(SymbolTable, AttributeNamesDedupeValuesDoNot) {
    SymbolTable st("aaa");
    EXPECT_EQ(st.new_attribute("a"), "#aaaa1");
    EXPECT_EQ(st.new_attribute("b"), "#aaaa2");
    EXPECT_EQ(st.new_attribute("a"), "#aaaa1");  // dedupes by real name

    EXPECT_EQ(st.new_value(num("1")), ":vaaa1");
    EXPECT_EQ(st.new_value(num("1")), ":vaaa2");  // values never dedupe

    auto names = st.get_names();  // token -> real name (inverted)
    EXPECT_EQ(names.at("#aaaa1"), "a");
    EXPECT_EQ(names.at("#aaaa2"), "b");
    EXPECT_EQ(st.get_values().size(), 2u);
}

TEST(SymbolTable, DottedPathTokenizedPerSegment) {
    SymbolTable st("aaa");
    EXPECT_EQ(st.new_attribute("a.b"), "#aaaa1.#aaaa2");
    EXPECT_EQ(st.new_attribute("a.c"), "#aaaa1.#aaaa3");  // "a" reused, "c" fresh
}

TEST(SymbolTable, SubscriptPreservedLiterally) {
    SymbolTable st("aaa");
    EXPECT_EQ(st.new_attribute("a[3]"), "#aaaa1[3]");
    EXPECT_EQ(st.new_attribute("a.b[2]"), "#aaaa1.#aaaa2[2]");
}

// ---- comparison serialization and desugaring -------------------------------

TEST(Comparison, SerializesWithSymbolTable) {
    SymbolTable st("aaa");
    auto c = eq("x", "=", num("5"));
    EXPECT_EQ(c->serialize(&st), "#aaaa1 = :vaaa1");
}

TEST(Comparison, NotEqualDesugarsToNegatedEquals) {
    SymbolTable st("aaa");
    auto c = eq("x", "!=", num("5"));
    EXPECT_TRUE(c->is_negated());
    EXPECT_EQ(c->serialize(&st), "NOT (#aaaa1 = :vaaa1)");
}

TEST(Comparison, GudKeyOnlyForNonNegatedEquals) {
    EXPECT_EQ(eq("x", "=", num("5"))->count_gud_key("x"), 1);
    EXPECT_EQ(eq("x", "!=", num("5"))->count_gud_key("x"), 0);
    EXPECT_EQ(eq("x", ">", num("5"))->count_gud_key("x"), 0);
    EXPECT_EQ(eq("x", "=", num("5"))->count_gud_key("y"), 0);
}

// ---- attribute_exists / not_exists -----------------------------------------

TEST(AttributeExists, NotExistsIsNegatedExists) {
    SymbolTable st("aaa");
    LogicalAttributeExists exists("x", true);
    LogicalAttributeExists not_exists("x", false);
    EXPECT_EQ(exists.serialize(&st), "attribute_exists(#aaaa1)");
    EXPECT_TRUE(not_exists.is_negated());
    SymbolTable st2("aaa");
    EXPECT_EQ(not_exists.serialize(&st2), "NOT (attribute_exists(#aaaa1))");
}

// ---- range predicates ------------------------------------------------------

TEST(Between, SerializesAsKeyLegalRangePredicate) {
    SymbolTable st("aaa");
    LogicalBetween b("rk", num("1"), num("9"));
    EXPECT_EQ(b.serialize(&st), "#aaaa1 BETWEEN :vaaa1 AND :vaaa2");
    EXPECT_EQ(b.count_query_rk("rk"), 1);
}

TEST(BeginsWith, SerializesWithFunctionForm) {
    SymbolTable st("aaa");
    LogicalBeginsWith bw("rk", str("abc"));
    EXPECT_EQ(bw.serialize(&st), "begins_with(#aaaa1, :vaaa1)");
}

// ---- Where capability probes (the GetItem/Query/Scan decision) -------------

TEST(Where, FullKeyEqualityIsGetItem) {
    // pk = 1 AND rk = 2  ->  GetItem. The planner probes can_getitem() FIRST,
    // so can_query_table() is never consulted for a full key; the original
    // encoded that ordering with assert(!can_getitem) inside can_query_table.
    // We drop the assert (D12), so can_query_table() also returns true here —
    // GetItem precedence is a planner-ordering contract, not mutual exclusion.
    auto tree = LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "=", num("2")));
    Where w(std::move(tree));
    EXPECT_TRUE(w.can_getitem("pk", "rk"));
}

TEST(Where, HashOnlyEqualityIsQuery) {
    // pk = 1 (table has only a hash key)  ->  Query
    Where w(eq("pk", "=", num("1")));
    EXPECT_TRUE(w.can_getitem("pk", ""));  // hash-only full key is a GetItem
}

TEST(Where, HashEqualityPlusRangePredicateIsQueryNotGetItem) {
    // pk = 1 AND rk > 5  ->  Query (not GetItem: rk is not an equality key)
    auto tree = LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", ">", num("5")));
    Where w(std::move(tree));
    EXPECT_FALSE(w.can_getitem("pk", "rk"));
    EXPECT_TRUE(w.can_query_table("pk", "rk"));
}

TEST(Where, RangeInequalityOnKeyIsNotQuerySafe) {
    // pk = 1 AND rk != 5  ->  Scan (a != on the range key is not key-legal)
    auto tree = LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "!=", num("5")));
    Where w(std::move(tree));
    EXPECT_FALSE(w.can_getitem("pk", "rk"));
    EXPECT_FALSE(w.can_query_table("pk", "rk"));
}

TEST(Where, PkEqualityUnderTopLevelOrForcesScan) {
    // (pk = 1 OR pk = 2)  ->  Scan (B-CLASS carry-forward: an OR referencing a
    // key attribute is not query-safe; the whole predicate is a scan filter).
    auto tree = LogicalOr::make_or(eq("pk", "=", num("1")), eq("pk", "=", num("2")));
    Where w(std::move(tree));
    EXPECT_FALSE(w.can_getitem("pk", "rk"));
    EXPECT_FALSE(w.can_query_table("pk", "rk"));
}

TEST(Where, QueryKeyConditionAndFilterSplit) {
    // pk = 1 AND attr = 7  ->  key condition "#...pk = :v", filter on attr
    SymbolTable st("aaa");
    auto tree = LogicalAnd::make_and(eq("pk", "=", num("1")), eq("attr", "=", num("7")));
    Where w(std::move(tree));
    std::string kce = w.query_key_condition_expression("pk", "rk", &st);
    std::string filter = w.query_filter_expression("pk", "rk", &st);
    EXPECT_EQ(kce, "#aaaa1 = :vaaa1");
    EXPECT_EQ(filter, "#aaaa2 = :vaaa2");
}

TEST(Where, GudKeyMapHoldsKeyAttributesOnly) {
    auto tree = LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "=", str("x")));
    Where w(std::move(tree));
    KeyMap key = w.get_gud_key("pk", "rk");
    ASSERT_EQ(key.size(), 2u);
    EXPECT_EQ(key.at("pk").type, Value::Type::N);
    EXPECT_EQ(key.at("pk").s, "1");
    EXPECT_EQ(key.at("rk").type, Value::Type::S);
    EXPECT_EQ(key.at("rk").s, "x");
}

// ---- AND flatten (RAII, no double free) ------------------------------------

TEST(LogicalAnd, FlattensSameTypeChain) {
    SymbolTable st("aaa");
    auto a = LogicalAnd::make_and(eq("a", "=", num("1")), eq("b", "=", num("2")));
    auto both = LogicalAnd::make_and(std::move(a), eq("c", "=", num("3")));
    EXPECT_EQ(both->serialize(&st), "#aaaa1 = :vaaa1 AND #aaaa2 = :vaaa2 AND #aaaa3 = :vaaa3");
    EXPECT_EQ(both->count(), 3);
}

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
#include "expressions/attribute_serialize.hpp"
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
    // A non-negated BETWEEN on a sort-key prefix attribute is query-safe.
    const std::vector<std::string> pk{"pk"};
    const std::vector<std::string> sk{"rk"};
    EXPECT_TRUE(b.query_safe(KeySchemaView{pk, sk}));
}

TEST(BeginsWith, SerializesWithFunctionForm) {
    SymbolTable st("aaa");
    LogicalBeginsWith bw("rk", str("abc"));
    EXPECT_EQ(bw.serialize(&st), "begins_with(#aaaa1, :vaaa1)");
}

// ---- attribute_type: N/S/B type-token rendering (B22) ----------------------

// attribute_type(path, type) renders as attribute_type(#a, :v) where :v is a
// STRING value carrying the DynamoDB scalar-type token "N"/"S"/"B" (the grammar
// maps NUMBER/STRING/BINARY to these tokens; arch §6). This mirrors the
// original CLogicalAttributeType, which built the value via
// `av.SetS(ScalarAttributeTypeMapper::GetNameForScalarAttributeType(...))`.
// Golden isel-9..63 pin the N and S forms at the EXPLAIN level; the BINARY ("B")
// branch is exercised by NO golden, so this case pins all three at the
// expression level — including the EAV value content (type S, token text) — to
// close the B22 verify-then-pin item.
TEST(AttributeType, RendersScalarTypeTokenAsStringValue) {
    for (const std::string& tok : {std::string("N"), std::string("S"), std::string("B")}) {
        SymbolTable st("aaa");
        LogicalAttributeType at("x", tok);
        EXPECT_EQ(at.serialize(&st), "attribute_type(#aaaa1, :vaaa1)");
        const auto& values = st.get_values();
        ASSERT_EQ(values.size(), 1u);
        const Value& v = values.at(":vaaa1");
        EXPECT_EQ(v.type, Value::Type::S);
        EXPECT_EQ(v.s, tok);
    }
}

// ---- serialize_attribute: the null-symbol-table literal serialize path (B21) -

// The ported LogicalExpression::serialize() accepts a null SymbolTable*, in
// which case right-hand Values serialize LITERALLY via serialize_attribute()
// (mirroring the original's AttributeValue::SerializeAttribute(), newlines
// stripped — arch §6/§10). VERIFIED (by inspection of every planner/executor
// serializer call site — select_planner.cpp, write_common.hpp,
// update_executor.cpp, delete_executor.cpp, transact_build.cpp): no in-scope
// command execution path ever passes a null symbol table, so this branch is
// unreachable in production. It is retained only for faithfulness of the ported
// base class (whose serialize() signature accepts a null table). These cases
// pin its byte output as the engine's own contract so the retained-for-fidelity
// code cannot silently rot. It is NOT golden-relevant (no command surfaces it).
TEST(SerializeAttribute, NullSymbolTablePathBytes) {
    EXPECT_EQ(serialize_attribute(Value::string_value("foo")), "{\"S\":\"foo\"}");
    EXPECT_EQ(serialize_attribute(Value::number_value("5")), "{\"N\":\"5\"}");
    EXPECT_EQ(serialize_attribute(Value::bool_value(true)), "{\"BOOL\":true}");
    EXPECT_EQ(serialize_attribute(Value::bool_value(false)), "{\"BOOL\":false}");
    EXPECT_EQ(serialize_attribute(Value::null_value()), "{\"NULL\":true}");

    Value list;
    list.type = Value::Type::L;
    list.list = {Value::number_value("3"), Value::string_value("x")};
    EXPECT_EQ(serialize_attribute(list), "{\"L\":[{\"N\":\"3\"},{\"S\":\"x\"}]}");

    Value map;
    map.type = Value::Type::M;
    map.map = {{"a", Value::number_value("1")}};
    EXPECT_EQ(serialize_attribute(map), "{\"M\":{\"a\":{\"N\":\"1\"}}}");

    // Embedded quote / newline are escaped (the newline becomes a two-char \n,
    // so the downstream newline-strip removes nothing).
    EXPECT_EQ(serialize_attribute(Value::string_value("a\"b")), "{\"S\":\"a\\\"b\"}");
    EXPECT_EQ(serialize_attribute(Value::string_value("a\nb")), "{\"S\":\"a\\nb\"}");
}

// ---- Where capability probes (the GetItem/Query/Scan decision) -------------

TEST(Where, FullKeyEqualityIsGetItem) {
    // pk = 1 AND rk = 2  ->  GetItem. The planner probes can_getitem() FIRST,
    // so plan_query() is never consulted for a full key; the original encoded
    // that ordering with assert(!can_getitem) inside can_query_table. We drop
    // the assert (D12), so plan_query() also succeeds here — GetItem precedence
    // is a planner-ordering contract, not mutual exclusion.
    auto tree = LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "=", num("2")));
    Where w(std::move(tree));
    EXPECT_TRUE(w.can_getitem({"pk"}, {"rk"}));
}

TEST(Where, HashOnlyEqualityIsQuery) {
    // pk = 1 (table has only a hash key)  ->  Query
    Where w(eq("pk", "=", num("1")));
    EXPECT_TRUE(w.can_getitem({"pk"}, {}));  // hash-only full key is a GetItem
}

TEST(Where, HashEqualityPlusRangePredicateIsQueryNotGetItem) {
    // pk = 1 AND rk > 5  ->  Query (not GetItem: rk is not an equality key)
    auto tree = LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", ">", num("5")));
    Where w(std::move(tree));
    EXPECT_FALSE(w.can_getitem({"pk"}, {"rk"}));
    EXPECT_TRUE(w.plan_query({"pk"}, {"rk"}).has_value());
}

TEST(Where, RangeNotEqualOnKeySplitsIntoTwoQueries) {
    // pk = 1 AND rk != 5  ->  two Queries, rk < 5 then rk > 5 (P15, D-P15-8
    // Rule 2; v0.8.1 planned a Scan, because a not-equal is not key-legal).
    for (const char* op : {"!=", "<>"}) {
        SCOPED_TRACE(op);
        auto tree = LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", op, num("5")));
        Where w(std::move(tree));
        EXPECT_FALSE(w.can_getitem({"pk"}, {"rk"}));
        auto plan = w.plan_query({"pk"}, {"rk"});
        ASSERT_TRUE(plan.has_value());
        ASSERT_TRUE(plan->ne_split.has_value());
        EXPECT_EQ(*plan->ne_split, "rk");
        SymbolTable lower("aaa");
        EXPECT_EQ(w.query_key_condition_expression(*plan, &lower, NeLeg::lower),
                  "#aaaa1 = :vaaa1 AND #aaaa2 < :vaaa2");
        SymbolTable upper("aaa");
        EXPECT_EQ(w.query_key_condition_expression(*plan, &upper, NeLeg::upper),
                  "#aaaa1 = :vaaa1 AND #aaaa2 > :vaaa2");
        SymbolTable filter("aaa");
        EXPECT_EQ(w.query_filter_expression(*plan, &filter), "");
    }
}

// ---- `<>` and `!=` are one operator (P15, D-P15-8 Rule 1) ---------------------

TEST(NotEqual, MeaningIsIndependentOfSpelling) {
    LogicalComparison eq_op("a", "=", num("1"));
    LogicalComparison bang("a", "!=", num("1"));
    LogicalComparison ltgt("a", "<>", num("1"));
    LogicalComparison not_ltgt("a", "<>", num("1"));
    not_ltgt.negate();
    LogicalComparison negated_eq("a", "=", num("1"));
    negated_eq.negate();
    EXPECT_TRUE(eq_op.means_equal());
    EXPECT_FALSE(eq_op.means_not_equal());
    EXPECT_TRUE(bang.means_not_equal());
    EXPECT_TRUE(ltgt.means_not_equal());
    EXPECT_TRUE(negated_eq.means_not_equal());
    EXPECT_TRUE(not_ltgt.means_equal());
    EXPECT_FALSE(not_ltgt.means_not_equal());
    LogicalComparison lt("a", "<", num("1"));
    EXPECT_FALSE(lt.means_equal());
    EXPECT_FALSE(lt.means_not_equal());
    // count_gud_key: `NOT a <> 1` is a key equality, like `NOT a != 1`.
    EXPECT_EQ(not_ltgt.count_gud_key("a"), 1);
    EXPECT_EQ(ltgt.count_gud_key("a"), 0);
    EXPECT_EQ(bang.count_gud_key("a"), 0);
}

TEST(NotEqual, FiltersKeepTheTypedSpelling) {
    SymbolTable st1("aaa");
    Where bang(eq("x", "!=", num("3")));
    EXPECT_EQ(bang.scan_filter_expression(&st1), "NOT (#aaaa1 = :vaaa1)");
    SymbolTable st2("aaa");
    Where ltgt(eq("x", "<>", num("3")));
    EXPECT_EQ(ltgt.scan_filter_expression(&st2), "#aaaa1 <> :vaaa1");
}

TEST(NotEqual, LtGtOnSortKeyIsNotAKeyConditionUnsplit) {
    // B6: v0.8.1 treated `rk <> 5` as query-safe and put it in the key
    // condition. Now it is safe only as the split attribute.
    auto tree = LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "<>", num("5")));
    const std::vector<std::string> pk{"pk"};
    const std::vector<std::string> rk{"rk"};
    EXPECT_FALSE(tree->query_safe(KeySchemaView{pk, rk}));
    const std::string split = "rk";
    EXPECT_TRUE(tree->query_safe(KeySchemaView{pk, rk, &split, NeLeg::lower}));
}

TEST(NotEqual, NotLtGtOnSortKeyIsGetItem) {
    // pk = 1 AND NOT rk <> 5 means rk = 5: a GetItem, like NOT rk != 5.
    for (const char* op : {"!=", "<>"}) {
        SCOPED_TRACE(op);
        auto ne = eq("rk", op, num("5"));
        ne->negate();
        Where w(LogicalAnd::make_and(eq("pk", "=", num("1")), std::move(ne)));
        EXPECT_TRUE(w.can_getitem({"pk"}, {"rk"}));
        KeyMap key = w.get_gud_key({"pk"}, {"rk"});
        EXPECT_EQ(key.size(), 2u);
    }
}

TEST(NotEqual, NotLtGtRendersAsEqualityInAKeyCondition) {
    // On an index (no GetItem), pk = 1 AND NOT rk <> 5 is a Query whose key
    // condition reads `rk = 5`, never `NOT (rk <> 5)`.
    auto ne = eq("rk", "<>", num("5"));
    ne->negate();
    Where w(LogicalAnd::make_and(eq("pk", "=", num("1")), std::move(ne)));
    auto plan = w.plan_query({"pk"}, {"rk"});
    ASSERT_TRUE(plan.has_value());
    EXPECT_FALSE(plan->ne_split.has_value());
    SymbolTable st("aaa");
    EXPECT_EQ(w.query_key_condition_expression(*plan, &st), "#aaaa1 = :vaaa1 AND #aaaa2 = :vaaa2");
}

TEST(NotEqual, NeKeyValueOnlyForATopLevelConjunct) {
    // Found in a plain AND chain.
    auto plain = LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "<>", num("5")));
    ASSERT_NE(plain->ne_key_value("rk"), nullptr);
    EXPECT_EQ(plain->ne_key_value("rk")->s, "5");
    EXPECT_EQ(plain->ne_key_value("pk"), nullptr);
    // Not under OR.
    auto under_or =
        LogicalAnd::make_and(eq("pk", "=", num("1")),
                             LogicalOr::make_or(eq("rk", "<>", num("5")), eq("x", "=", num("1"))));
    EXPECT_EQ(under_or->ne_key_value("rk"), nullptr);
    // Not under a negated AND.
    auto negated = LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "!=", num("5")));
    negated->negate();
    EXPECT_EQ(negated->ne_key_value("rk"), nullptr);
    // A negated not-equal means equal.
    auto not_ne = eq("rk", "<>", num("5"));
    not_ne->negate();
    EXPECT_EQ(not_ne->ne_key_value("rk"), nullptr);
}

TEST(NotEqual, NoSplitWhenTheSortKeyIsReferencedTwice) {
    // pk = 1 AND rk <> 5 AND rk > 2: the open range (2, 5) needs a second
    // sort-key condition, so it stays a Scan.
    auto tree = LogicalAnd::make_and(
        LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "<>", num("5"))),
        eq("rk", ">", num("2")));
    Where w(std::move(tree));
    EXPECT_FALSE(w.plan_query({"pk"}, {"rk"}).has_value());
}

TEST(NotEqual, NoSplitUnderOrOrNegatedAnd) {
    Where under_or(
        LogicalAnd::make_and(eq("pk", "=", num("1")),
                             LogicalOr::make_or(eq("rk", "<>", num("5")), eq("x", "=", num("1")))));
    EXPECT_FALSE(under_or.plan_query({"pk"}, {"rk"}).has_value());
    auto negated = LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "<>", num("5")));
    negated->negate();
    Where w(std::move(negated));
    EXPECT_FALSE(w.plan_query({"pk"}, {"rk"}).has_value());
}

TEST(NotEqual, NoSplitOnThePartitionKey) {
    Where w(LogicalAnd::make_and(eq("pk", "<>", num("1")), eq("rk", "=", num("5"))));
    EXPECT_FALSE(w.plan_query({"pk"}, {"rk"}).has_value());
}

TEST(NotEqual, TypeRule) {
    // The literal must be a scalar of the key's declared type; otherwise the
    // statement keeps v0.8.1's Scan (a mistyped key condition is a
    // ValidationException).
    const KeyTypes number_rk{{"pk", ScalarType::n}, {"rk", ScalarType::n}};
    {
        Where w(LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "<>", str("x"))));
        EXPECT_FALSE(w.plan_query({"pk"}, {"rk"}, &number_rk).has_value());
        // With no declared type, any S/N/B literal splits.
        EXPECT_TRUE(w.plan_query({"pk"}, {"rk"}).has_value());
    }
    {
        Where w(LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "<>", num("7"))));
        auto plan = w.plan_query({"pk"}, {"rk"}, &number_rk);
        ASSERT_TRUE(plan.has_value());
        EXPECT_TRUE(plan->ne_split.has_value());
    }
    for (Value v : {Value{}, Value::bool_value(true)}) {
        Where w(LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "!=", v)));
        EXPECT_FALSE(w.plan_query({"pk"}, {"rk"}).has_value());
    }
    {
        // An unbound parameter marker is not a value yet.
        Value marker;
        marker.param_ordinal = 1;
        Where w(LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "<>", marker)));
        EXPECT_FALSE(w.plan_query({"pk"}, {"rk"}).has_value());
    }
}

TEST(NotEqual, CompositeSortKeySplitsOnlyOnTheLastComponent) {
    // GSI ((g) HASH, (h, r) RANGE).
    {
        Where w(LogicalAnd::make_and(eq("g", "=", str("a")), eq("h", "<>", num("5"))));
        auto plan = w.plan_query({"g"}, {"h", "r"});
        ASSERT_TRUE(plan.has_value());
        EXPECT_EQ(plan->ne_split.value_or(""), "h");
    }
    {
        Where w(LogicalAnd::make_and(
            LogicalAnd::make_and(eq("g", "=", str("a")), eq("h", "=", num("5"))),
            eq("r", "<>", num("5"))));
        auto plan = w.plan_query({"g"}, {"h", "r"});
        ASSERT_TRUE(plan.has_value());
        EXPECT_EQ(plan->ne_split.value_or(""), "r");
        SymbolTable st("aaa");
        EXPECT_EQ(w.query_key_condition_expression(*plan, &st, NeLeg::upper),
                  "#aaaa1 = :vaaa1 AND #aaaa2 = :vaaa2 AND #aaaa3 > :vaaa3");
    }
    {
        Where w(LogicalAnd::make_and(
            LogicalAnd::make_and(eq("g", "=", str("a")), eq("h", "<>", num("5"))),
            eq("r", "=", num("5"))));
        EXPECT_FALSE(w.plan_query({"g"}, {"h", "r"}).has_value());
    }
}

TEST(NotEqual, SplitKeepsNonKeyPredicatesInTheFilter) {
    Where w(LogicalAnd::make_and(
        LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "<>", num("5"))),
        eq("x", "<>", num("3"))));
    auto plan = w.plan_query({"pk"}, {"rk"});
    ASSERT_TRUE(plan.has_value());
    SymbolTable st("aaa");
    EXPECT_EQ(w.query_key_condition_expression(*plan, &st, NeLeg::lower),
              "#aaaa1 = :vaaa1 AND #aaaa2 < :vaaa2");
    EXPECT_EQ(w.query_filter_expression(*plan, &st), "#aaaa3 <> :vaaa3");
}

TEST(NotEqual, ConditionCheckKeepsANotEqualKeyLeafInBothSpellings) {
    // UPDATE ... WHERE pk = 1 AND rk = 2 AND rk <> 2: the not-equal stays in
    // the condition, as `rk != 2` always did (site 9).
    {
        SymbolTable st("aaa");
        Where w(LogicalAnd::make_and(
            LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "=", num("2"))),
            eq("rk", "<>", num("2"))));
        EXPECT_EQ(w.update_delete_condition_check("pk", "rk", &st), "#aaaa1 <> :vaaa1");
    }
    {
        SymbolTable st("aaa");
        Where w(LogicalAnd::make_and(
            LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "=", num("2"))),
            eq("rk", "!=", num("2"))));
        EXPECT_EQ(w.update_delete_condition_check("pk", "rk", &st), "NOT (#aaaa1 = :vaaa1)");
    }
    {
        // NOT rk <> 2 means rk = 2: a plain key comparison, dropped.
        SymbolTable st("aaa");
        auto ne = eq("rk", "<>", num("2"));
        ne->negate();
        Where w(LogicalAnd::make_and(eq("pk", "=", num("1")), std::move(ne)));
        EXPECT_EQ(w.update_delete_condition_check("pk", "rk", &st), "");
    }
}

TEST(Where, PkEqualityUnderTopLevelOrForcesScan) {
    // (pk = 1 OR pk = 2)  ->  Scan (B-CLASS carry-forward: an OR referencing a
    // key attribute is not query-safe; the whole predicate is a scan filter).
    auto tree = LogicalOr::make_or(eq("pk", "=", num("1")), eq("pk", "=", num("2")));
    Where w(std::move(tree));
    EXPECT_FALSE(w.can_getitem({"pk"}, {"rk"}));
    EXPECT_FALSE(w.plan_query({"pk"}, {"rk"}).has_value());
}

TEST(Where, QueryKeyConditionAndFilterSplit) {
    // pk = 1 AND attr = 7  ->  key condition "#...pk = :v", filter on attr
    SymbolTable st("aaa");
    auto tree = LogicalAnd::make_and(eq("pk", "=", num("1")), eq("attr", "=", num("7")));
    Where w(std::move(tree));
    auto plan = w.plan_query({"pk"}, {"rk"});
    ASSERT_TRUE(plan.has_value());
    std::string kce = w.query_key_condition_expression(*plan, &st);
    std::string filter = w.query_filter_expression(*plan, &st);
    EXPECT_EQ(kce, "#aaaa1 = :vaaa1");
    EXPECT_EQ(filter, "#aaaa2 = :vaaa2");
}

TEST(Where, GudKeyMapHoldsKeyAttributesOnly) {
    auto tree = LogicalAnd::make_and(eq("pk", "=", num("1")), eq("rk", "=", str("x")));
    Where w(std::move(tree));
    KeyMap key = w.get_gud_key({"pk"}, {"rk"});
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
